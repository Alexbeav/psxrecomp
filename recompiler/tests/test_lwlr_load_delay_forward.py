#!/usr/bin/env python3
"""Check CFG emission uses CPU-owned LWL/LWR forwarding; test_load_delay_l1 executes it."""
import argparse, glob, os, re, struct, subprocess, sys, tempfile

LOAD = 0x80010000

# lw   $t0, 12($a3)   opcode 0x23 rs=7 rt=8 imm=0x000C
LW_T0_A3_12 = 0x8CE8000C
# lwr  $t0, 10($a3)   opcode 0x26 rs=7 rt=8 imm=0x000A
LWR_T0_A3_10 = 0x98E8000A
JR_RA = 0x03E00008
NOP = 0x00000000


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)
    return bytes(h) + data


def build_exe():
    # One function: the dependent lw / lwr pair, then return.
    return make_psxexe(LOAD, LOAD, w([LW_T0_A3_12, LWR_T0_A3_10, JR_RA, NOP]))


def generate(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(build_exe())
    with open(seeds, "w") as f:
        f.write("0x%08X\n" % LOAD)
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
    srcs = [p for p in glob.glob(os.path.join(out, "*_full*.c"))]
    if not srcs:
        raise SystemExit("no *_full*.c emitted in " + out)
    return "".join(open(p, encoding="utf-8", errors="replace").read() for p in srcs)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build",
                                                          "psxrecomp-game")))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        src = generate(args.recompiler, tmp)

    assert "psx_load_value_arm(cpu, 8u" in src, "load did not arm CPUState"
    assert "psx_load_value_merge(cpu, 8u)" in src, "merge ignored pending value"
    assert "psx_ldd_" not in src, "block-local delayed value still emitted"
    print("LWL/LWR forwards the CPU-owned pending value")


if __name__ == "__main__":
    main()
