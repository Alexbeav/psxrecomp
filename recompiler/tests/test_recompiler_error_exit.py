#!/usr/bin/env python3
"""psxrecomp-game reports bad input with exit status 1, never abort().

Errors reach main() as exceptions: a malformed game.toml (here: a bad
[widescreen.cull] clip_edge_width, a non-hex or non-string site address) and an
input the emitter must refuse (an overlay whose mandatory delay slot lies
outside the image). Uncaught, they ended in abort(), which exits by signal and
leaves a macOS crash report; a returncode != 0 check cannot tell the two
apart, so this test requires exactly 1 and the diagnostic. Main-EXE
[widescreen.cull] sites that hold the wrong opcode exit 1 through their own
guard and are checked the same way.

It also checks that one valid bgez site still generates, so a harness problem
cannot pass as a rejection.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

LOAD = 0x80010000
# bgez v0,+2 ; nop ; addiu v1,zero,1 ; jr ra ; nop
WORDS = [0x04410002, 0x00000000, 0x24030001, 0x03E00008, 0x00000000]

BASE = """[game]
name = "Cull Config Errors"
id = "TEST-00000"
exe = "t.psx"
entry_pc = "0x80010000"
text_size = "0x14"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "out"

[widescreen.cull]
"""

# (name, [widescreen.cull] body, expected exit status, stderr needle)
CASES = [
    ("clip_edge_width_zero",
     'clip_edge_width = 0\nclip_edge_x_load_sites = ["0x80010008"]\n',
     1, "clip_edge_width must be 1..1024"),
    ("clip_edge_width_too_big",
     'clip_edge_width = 1025\nclip_edge_x_load_sites = ["0x80010008"]\n',
     1, "clip_edge_width must be 1..1024"),
    ("clip_edge_width_not_integer",
     'clip_edge_width = "wide"\nclip_edge_x_load_sites = ["0x80010008"]\n',
     1, "ERROR:"),
    ("bgez_site_not_hex",
     'bgez_sites = ["nothex"]\n',
     1, "widescreen.cull.bgez_sites"),
    ("clip_edge_site_not_hex",
     'clip_edge_x_load_sites = ["nothex"]\n',
     1, "widescreen.cull.clip_edge_x_load_sites"),
    ("bgez_site_not_string",
     "bgez_sites = [65536]\n",
     1, "ERROR:"),
    ("bgez_site_wrong_opcode",
     'bgez_sites = ["0x80010008"]\n',
     1, "bgez site 0x80010008 is not bgez"),
    ("clip_edge_site_wrong_opcode",
     'clip_edge_x_load_sites = ["0x80010008"]\n',
     1, "clip_edge_x_load site 0x80010008 is not lh/lhu/lw"),
    ("bgez_site_valid",
     'bgez_sites = ["0x80010000"]\n',
     0, None),
]


def make_psxexe() -> bytes:
    data = b"".join(struct.pack("<I", w) for w in WORDS)
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD)
    struct.pack_into("<I", header, 0x18, LOAD)
    struct.pack_into("<I", header, 0x1C, len(data))
    return bytes(header) + data


def run_case(recompiler, project_root, root, name, body, flag):
    case_dir = os.path.join(root, f"{name}{flag.replace('-', '_')}")
    out_dir = os.path.join(case_dir, "out")
    os.makedirs(out_dir)
    with open(os.path.join(case_dir, "t.psx"), "wb") as f:
        f.write(make_psxexe())
    with open(os.path.join(case_dir, "seeds.txt"), "w", encoding="ascii") as f:
        f.write(f"0x{LOAD:08X}\n")
    config = os.path.join(case_dir, "game.toml")
    with open(config, "w", encoding="ascii") as f:
        f.write(BASE + body)
    if flag == "--config":
        cmd = [recompiler, "--config", config, "--project-root", project_root]
    else:
        cmd = [recompiler, os.path.join(case_dir, "t.psx"),
               "--seeds", os.path.join(case_dir, "seeds.txt"),
               "--out-dir", out_dir, "--project-root", project_root,
               "--ws-config", config]
    proc = subprocess.run(cmd, cwd=case_dir, capture_output=True, text=True)
    return proc, out_dir


def run_truncated_overlay(recompiler, project_root, root):
    """An overlay page ending in a branch whose delay slot is not in the
    image: generation must fail closed (see
    test_overlay_cross_page_delay_codegen.py)."""
    case_dir = os.path.join(root, "truncated_overlay")
    out_dir = os.path.join(case_dir, "out")
    os.makedirs(out_dir)
    load = 0x80010FF0
    words = [0, 0, 0, 0x10800008]  # nops; beq a0,zero,+8 with no delay slot
    data = b"".join(struct.pack("<I", w) for w in words)
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, load)
    struct.pack_into("<I", header, 0x18, load)
    struct.pack_into("<I", header, 0x1C, len(data))
    psx = os.path.join(case_dir, "cross_page.psx")
    with open(psx, "wb") as f:
        f.write(bytes(header) + data)
    seeds = os.path.join(case_dir, "seeds.txt")
    with open(seeds, "w", encoding="ascii") as f:
        f.write(f"dispatch_root 0x{load:08X}\n")
    return subprocess.run(
        [recompiler, psx, "--seeds", seeds, "--out-dir", out_dir, "--overlay",
         "--project-root", project_root],
        cwd=case_dir, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True)
    parser.add_argument("--project-root", required=True,
                        help="psxrecomp root (holds bios/SCPH1001.toml)")
    args = parser.parse_args()
    args.recompiler = os.path.abspath(args.recompiler)
    args.project_root = os.path.abspath(args.project_root)
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    failures = []
    with tempfile.TemporaryDirectory() as root:
        for name, body, want_rc, needle in CASES:
            for flag in ("--config", "--ws-config"):
                proc, out_dir = run_case(args.recompiler, args.project_root,
                                         root, name, body, flag)
                label = f"{name} ({flag})"
                if proc.returncode != want_rc:
                    how = (f"signal {-proc.returncode}" if proc.returncode < 0
                           else f"status {proc.returncode}")
                    failures.append(f"{label}: exited with {how}, want status "
                                    f"{want_rc}\n{proc.stderr[-600:]}")
                    continue
                if needle is not None and needle not in proc.stderr:
                    failures.append(f"{label}: stderr lacks {needle!r}\n"
                                    f"{proc.stderr[-600:]}")
                if want_rc == 0:
                    generated = ""
                    for fname in os.listdir(out_dir):
                        if fname.endswith(".c"):
                            with open(os.path.join(out_dir, fname),
                                      encoding="utf-8") as f:
                                generated += f.read()
                    if "psx_ws_cull_bgez(" not in generated:
                        failures.append(f"{label}: valid bgez site was not "
                                        "emitted through psx_ws_cull_bgez")

        proc = run_truncated_overlay(args.recompiler, args.project_root, root)
        if proc.returncode != 1:
            how = (f"signal {-proc.returncode}" if proc.returncode < 0
                   else f"status {proc.returncode}")
            failures.append(f"truncated overlay: exited with {how}, want "
                            f"status 1\n{proc.stderr[-600:]}")
        elif "ERROR:" not in proc.stderr or \
                "mandatory delay slot" not in proc.stderr:
            failures.append("truncated overlay: stderr lacks the ERROR: "
                            f"mandatory delay slot diagnostic\n"
                            f"{proc.stderr[-600:]}")

    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"PASS: {len(CASES) * 2} widescreen.cull config cases and a "
          "truncated overlay exit 1 with a message; a valid site generates")
    return 0


if __name__ == "__main__":
    sys.exit(main())
