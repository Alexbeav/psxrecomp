#!/usr/bin/env python3
"""run.py — run the BIOS resume-PC test ROM on psx-bresume and psx-beetle (T110).

Starts both processes on the same BIOS and disc, waits until each writes the
done magic, then checks:
  1. the timing-independent RESULT words match between the backends;
  2. the syscall and phase-1 loop counts equal the generator's constants;
  3. psx-bresume ran the interrupt callback at least 1000 times, so the
     recompiled kernel really was under interrupt load (psx-beetle's count is
     recorded but not gated: with the genuine BIOS it delivers no root-counter-2
     interrupts at all while psx-bresume delivers ~93k, an unresolved timer2
     divergence that is not what this test is for);
  4. psx-bresume recorded no unknown dispatch, and publish_ring holds no
     entry that psx_is_dispatchable refused;
  5. both backends ran the same kernel image (RAM 0x500..0x1500 compared).

psx-beetle loads firmware BY NAME from the directory its BIOS argument points at
(scph5500/scph5501/scph5502.bin by disc region), so --beetle-bios must name a
file in a directory holding the intended image under that region's name. Without
it psx-beetle reports "Firmware is missing" and runs a different kernel, which
silently voids the comparison.

Usage:
  python tools/bios_resume_testrom/run.py --runtime <build/BIOS_Resume_Test_ROM.exe> \
      --beetle <psx-beetle.exe> --bios bios/EUR-PSX-SCPH5552.bin \
      [--env PSX_PRECISE_SLICE=1] [--report out.json]
Exit status 0 = pass. The JSON report records every value read.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cycle_testrom"))
from measure import query  # noqa: E402

RUNTIME_PORT = 4610
BEETLE_PORT = 4382


def read_words(port, addr, count):
    r = query(port, {"id": 1, "cmd": "read_ram", "addr": f"0x{addr:08X}", "len": 4 * count})
    raw = bytes.fromhex(r["hex"])
    return [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)]


def wait_done(port, result, magic, deadline):
    last = None
    while time.time() < deadline:
        try:
            words = read_words(port, result, 10)
            last = words
            if words[8] == magic:
                return words
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(1.0)
    return last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--beetle", required=True)
    ap.add_argument("--bios", required=True)
    ap.add_argument("--beetle-bios", help="path inside psx-beetle's system dir "
                    "(region-named copy of the same image); defaults to --bios")
    ap.add_argument("--disc", default=os.path.join(HERE, "disc", "bresume.cue"))
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for psx-bresume")
    ap.add_argument("--timeout", type=float, default=240.0)
    ap.add_argument("--report")
    args = ap.parse_args()
    args.runtime, args.beetle = os.path.abspath(args.runtime), os.path.abspath(args.beetle)
    args.bios, args.disc = os.path.abspath(args.bios), os.path.abspath(args.disc)
    args.beetle_bios = os.path.abspath(args.beetle_bios or args.bios)

    with open(os.path.join(HERE, "bios_resume_testrom.exe.json"), encoding="utf-8") as fh:
        meta = json.load(fh)
    result = int(meta["result"], 16)
    magic = int(meta["done_magic"], 16)

    env = dict(os.environ)
    for kv in args.env:
        k, v = kv.split("=", 1)
        env[k] = v
    procs = [
        subprocess.Popen([args.runtime, "--no-launcher", "--headless", "--game", os.path.join(HERE, "game.toml"),
                          "--bios", args.bios, "--disc", args.disc],
                         cwd=os.path.dirname(os.path.abspath(args.runtime)), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([args.beetle, args.beetle_bios, "--disc", args.disc, "--port", str(BEETLE_PORT)],
                         cwd=os.path.dirname(os.path.abspath(args.beetle)),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    ]
    report = {"bios": os.path.basename(args.bios),
              "beetle_bios": args.beetle_bios, "env": args.env, "checks": {}}
    try:
        deadline = time.time() + args.timeout
        native = wait_done(RUNTIME_PORT, result, magic, deadline)
        beetle = wait_done(BEETLE_PORT, result, magic, deadline)
        report["native"] = [f"0x{w:08X}" for w in native] if native else None
        report["beetle"] = [f"0x{w:08X}" for w in beetle] if beetle else None
        c = report["checks"]
        c["native_done"] = bool(native) and native[8] == magic
        c["beetle_done"] = bool(beetle) and beetle[8] == magic
        if c["native_done"] and c["beetle_done"]:
            c["results_match"] = native[4:8] == beetle[4:8]
            c["counts_expected"] = (native[5] == meta["expect"]["0x14"] and
                                    native[6] == meta["expect"]["0x18"])
            # Callback counts are timing. The property under test is that the
            # RECOMPILED runtime took a sustained interrupt load while running
            # BIOS kernel code, so only the native count gates. The oracle count
            # is reported for the timer2 divergence noted above.
            c["native_irqs_taken"] = native[0] >= 1000
            report["beetle_irq_count"] = beetle[0]
        try:
            band = [query(p, {"id": 1, "cmd": "read_ram", "addr": "0x00000500", "len": 4096},
                          )["hex"] for p in (RUNTIME_PORT, BEETLE_PORT)]
            a, b = (bytes.fromhex(x) for x in band)
            same = sum(1 for x, y in zip(a, b) if x == y)
            report["kernel_band_match_pct"] = round(100.0 * same / len(a), 2)
            # >=95% is the oracle bundle's "genuine same image" threshold; a wrong
            # firmware lands near 15-20%.
            c["same_kernel_image"] = report["kernel_band_match_pct"] >= 95.0
        except (OSError, ValueError, KeyError) as exc:
            report["kernel_band_error"] = str(exc)
            c["same_kernel_image"] = False
        try:
            unk = query(RUNTIME_PORT, {"id": 1, "cmd": "unknown_dispatch_log"})
            pub = query(RUNTIME_PORT, {"id": 1, "cmd": "publish_ring", "count": 1024, "only_bad": 1})
            allpub = query(RUNTIME_PORT, {"id": 1, "cmd": "publish_ring", "count": 1})
            report["unknown_dispatch_total"] = unk.get("total")
            report["publish_total"] = allpub.get("total")
            report["publish_refused"] = pub.get("entries")
            c["no_unknown_dispatch"] = unk.get("total") == 0
            c["no_refused_publish"] = pub.get("emitted") == 0
        except OSError as exc:
            report["runtime_query_error"] = str(exc)
            c["no_unknown_dispatch"] = False
    finally:
        for p in procs:
            p.kill()
    report["pass"] = bool(report["checks"]) and all(report["checks"].values())
    text = json.dumps(report, indent=2)
    print(text)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    sys.exit(0 if report["pass"] else 1)


if __name__ == "__main__":
    main()
