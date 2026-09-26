#!/usr/bin/env python3
"""run.py — run the cross-executor load-delay test ROM.

One boot of psx-xdiff runs the case body twice: from the compiled text image
(native) and from a guest-made copy outside the image (dirty-RAM interpreter).
The runner waits for the done magic, reads both RESULT blocks and the
interpreter's per-PC counters, and checks:
  1. the copy really ran in the interpreter (dirty_ram_stats has PCs in the copy);
  2. native and interpreted register words agree for every case;
  3. with --beetle, both also match psx-beetle's result for the same disc.
Word 7 of each case is a root-counter-2 delta. It is reported (and compared
against the reference when it runs) but does not gate native vs interpreted:
the two executors charge cycles differently.

Usage:
  python tools/executor_diff_testrom/run.py --runtime <Executor_Diff_Test_ROM.exe> \
      --bios <SCPH1001.BIN> [--beetle <psx-beetle.exe> --beetle-bios <dir/scph5501.bin>] \
      [--report out.json]
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

RUNTIME_PORT = 4611
BEETLE_PORT = 4382


def read_words(port, addr, count):
    r = query(port, {"id": 1, "cmd": "read_ram", "addr": f"0x{addr:08X}", "len": 4 * count})
    raw = bytes.fromhex(r["hex"])
    return [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)]


def run_one(cmd, cwd, env, port, meta, timeout, want_stats):
    done_addr, magic = int(meta["done_addr"], 16), int(meta["done_magic"], 16)
    nwords = len(meta["cases"]) * meta["words_per_case"]
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    out = {"done": False}
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                if read_words(port, done_addr, 1)[0] == magic:
                    out["done"] = True
                    break
            except (OSError, ValueError, KeyError):
                pass
            time.sleep(0.5)
        if out["done"]:
            out["native"] = read_words(port, int(meta["result_native"], 16), nwords)
            out["interp"] = read_words(port, int(meta["result_interp"], 16), nwords)
            if want_stats:
                out["stats"] = query(port, {"id": 1, "cmd": "dirty_ram_stats"})
    finally:
        proc.kill()
        proc.wait()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--bios", required=True)
    ap.add_argument("--beetle")
    ap.add_argument("--beetle-bios", help="region-named copy of the same image in psx-beetle's "
                    "firmware directory (scph5501.bin for this US-licensed disc)")
    ap.add_argument("--disc", default=os.path.join(HERE, "disc", "xdiff.cue"))
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--report")
    args = ap.parse_args()

    with open(os.path.join(HERE, "executor_diff_testrom.exe.json"), encoding="utf-8") as fh:
        meta = json.load(fh)
    per, tw = meta["words_per_case"], meta["timer_word"]
    copy_lo = int(meta["copy"], 16) & 0x1FFFFFFF
    copy_hi = copy_lo + 4 * meta["body_words"]

    runtime = os.path.abspath(args.runtime)
    env = {k: v for k, v in os.environ.items() if not k.startswith("PSX_FORCE")}
    rt = run_one([runtime, "--no-launcher", "--headless", "--game", os.path.join(HERE, "game.toml"),
                  "--bios", os.path.abspath(args.bios), "--disc", os.path.abspath(args.disc)],
                 os.path.dirname(runtime), env, RUNTIME_PORT, meta, args.timeout, True)
    bt = None
    if args.beetle:
        beetle = os.path.abspath(args.beetle)
        bt = run_one([beetle, os.path.abspath(args.beetle_bios or args.bios),
                      "--disc", os.path.abspath(args.disc), "--port", str(BEETLE_PORT)],
                     os.path.dirname(beetle), dict(os.environ), BEETLE_PORT, meta,
                     args.timeout, False)

    report = {"checks": {"runtime_done": rt["done"]}, "cases": {}}
    if bt is not None:
        report["checks"]["beetle_done"] = bt["done"]
    if rt["done"]:
        per_pc = rt["stats"].get("per_pc") or []
        copy_pcs = [p for p in per_pc if copy_lo <= (int(p["pc"], 16) & 0x1FFFFFFF) < copy_hi]
        report["interp_copy_pcs"] = copy_pcs
        report["interp_totals"] = {k: rt["stats"].get(k) for k in ("blocks_run", "insns_run")}
        report["checks"]["copy_ran_interpreted"] = bool(copy_pcs)
        for name, off in meta["cases"].items():
            i = int(off, 16) // 4
            row = {"native": [f"0x{w:08X}" for w in rt["native"][i:i + per]],
                   "interp": [f"0x{w:08X}" for w in rt["interp"][i:i + per]]}
            row["native_eq_interp"] = rt["native"][i:i + tw] == rt["interp"][i:i + tw]
            report["checks"][f"{name}_native_eq_interp"] = row["native_eq_interp"]
            if bt is not None and bt["done"]:
                # psx-beetle runs the same body twice as well; both copies must match.
                row["beetle"] = [f"0x{w:08X}" for w in bt["native"][i:i + per]]
                row["native_eq_beetle"] = rt["native"][i:i + tw] == bt["native"][i:i + tw]
                row["interp_eq_beetle"] = rt["interp"][i:i + tw] == bt["native"][i:i + tw]
                report["checks"][f"{name}_native_eq_beetle"] = row["native_eq_beetle"]
                report["checks"][f"{name}_interp_eq_beetle"] = row["interp_eq_beetle"]
            report["cases"][name] = row
    report["pass"] = all(report["checks"].values())
    text = json.dumps(report, indent=2)
    print(text)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    sys.exit(0 if report["pass"] else 1)


if __name__ == "__main__":
    main()
