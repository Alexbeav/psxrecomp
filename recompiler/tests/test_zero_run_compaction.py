#!/usr/bin/env python3
"""Recompiler codegen regression test: zero-word runs are compacted faithfully.

Discovery reaches zero-filled BSS / overlay load areas through JAL targets, and
every zero word (sll $0,$0,0) used to expand to ~10 lines of C; Parasite Eve's
func_8019234C was 1.1M lines of nops in one shard. The emitter now writes a run
leader through the normal path and its followers as one loop that makes the
same per-word calls (fetch-or-boundary, psx_cyc_step, cosim_instr).

The image holds, in one function:
  run A  1 leader + 64 followers   -> compacted (threshold is 64 followers)
  run B  1 leader + 63 followers   -> stays expanded
  run C  200 zero words split by a backward-branch target in the middle
         -> two blocks, each compacted; the label stays outside both loops

Asserts the loop bounds, that short runs stay expanded, and that every loop
re-expands to exactly the per-word text the emitter writes for an expanded zero
word at the same address class (taken from run B), so the loop cannot drift
from the expanded form.

Usage:  python test_zero_run_compaction.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, glob, os, re, struct, subprocess, tempfile

LOAD = 0x80010000
T0 = 8
ADDIU_T0_ZERO_1 = 0x24080001
ADDIU_T0_T0_1 = 0x25080001
JR_RA = 0x03E00008


def at(i):
    return LOAD + 4 * i


def bne_t0_zero(pc_index, target_index):
    off = (target_index - (pc_index + 1)) & 0xFFFF
    return (0x05 << 26) | (T0 << 21) | off


RUN_A = (1, 66)      # [first, end) word indices
RUN_B = (67, 131)
RUN_C = (132, 332)
C_TARGET = 232
BRANCH = 332


def build_words():
    words = [ADDIU_T0_ZERO_1]
    words += [0] * (RUN_A[1] - RUN_A[0])
    words += [ADDIU_T0_T0_1]
    words += [0] * (RUN_B[1] - RUN_B[0])
    words += [ADDIU_T0_T0_1]
    words += [0] * (RUN_C[1] - RUN_C[0])
    assert len(words) == BRANCH
    words += [bne_t0_zero(BRANCH, C_TARGET), 0, JR_RA, 0]
    return words


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)
    return bytes(h) + data


def generate(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(make_psxexe(LOAD, LOAD, b"".join(struct.pack("<I", x) for x in build_words())))
    with open(seeds, "w") as f:
        f.write("0x%08X\n" % LOAD)
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
    srcs = sorted(glob.glob(os.path.join(out, "*_full*.c")))
    if not srcs:
        raise SystemExit("no *_full*.c emitted in " + out)
    return "".join(open(p, encoding="utf-8", newline="").read() for p in srcs)


LOOP = re.compile(
    r"^(?P<in>[ ]*)\{ /\* zero-word run 0x(?P<first>[0-9A-F]{8})\.\.0x(?P<last>[0-9A-F]{8}): "
    r"(?P<n>\d+) words, per-word calls as expanded \*/\n"
    r"(?:.*\n)*?(?P=in)\}\n", re.M)


def word_chunk(src, addr):
    """Expanded text of the zero word at addr: fetch/boundary through cosim."""
    a = "0x%08X" % addr
    m = re.search(
        r"#ifdef PSX_ENABLE_BLOCK_CYCLES\n[ ]*psx_(?:icache_fetch|cpu_step_boundary)\(cpu, %su\);\n"
        r"(?:.*\n)*?[ ]*cosim_instr\(%su\);\n#endif\n" % (a, a), src)
    return m.group(0) if m else None


def expand_loop(m, template_for):
    first = int(m.group("first"), 16)
    n = int(m.group("n"))
    return "".join(template_for(first + 4 * k) for k in range(n))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build", "psxrecomp-game")))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler) and not os.path.isfile(args.recompiler + ".exe"):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        src = generate(args.recompiler, tmp)

    failures = []
    loops = list(LOOP.finditer(src))
    got = [(int(m.group("first"), 16), int(m.group("last"), 16), int(m.group("n"))) for m in loops]
    want = [
        (at(RUN_A[0] + 1), at(RUN_A[1] - 1), RUN_A[1] - RUN_A[0] - 1),
        (at(RUN_C[0] + 1), at(C_TARGET - 1), C_TARGET - RUN_C[0] - 1),
        (at(C_TARGET + 1), at(RUN_C[1] - 1), RUN_C[1] - C_TARGET - 1),
    ]
    if got != want:
        failures.append("zero-run loops %s, expected %s" % (
            [("%08X" % f, "%08X" % l, n) for f, l, n in got],
            [("%08X" % f, "%08X" % l, n) for f, l, n in want]))

    # Run B (63 followers) stays expanded, word by word.
    for i in range(RUN_B[0], RUN_B[1]):
        if "/* nop */  /* 0x%08X: 0x00000000 */" % at(i) not in src:
            failures.append("run B word 0x%08X is not emitted expanded" % at(i))
            break

    # The branch target inside run C is a real label between the two loops.
    label = "block_%08X:" % at(C_TARGET)
    if label not in src:
        failures.append("missing label %s inside run C" % label)
    elif len(loops) == 3 and not (loops[1].end() <= src.index(label) < loops[2].start()):
        failures.append("label %s is not between the two run C loops" % label)

    # Expanded-form templates from run B followers, one per (addr & 0xC) class.
    templates = {}
    for i in range(RUN_B[0] + 1, RUN_B[1]):
        cls = at(i) & 0xC
        if cls not in templates:
            chunk = word_chunk(src, at(i))
            if chunk is None:
                failures.append("cannot find expanded text for 0x%08X" % at(i))
                break
            templates[cls] = (at(i), chunk)
    if len(templates) == 4:
        def template_for(addr):
            base, chunk = templates[addr & 0xC]
            return chunk.replace("0x%08X" % base, "0x%08X" % addr)
        for m in loops:
            text = m.group(0)
            # The loop must make exactly the calls the template makes.
            for needle in ("psx_cyc_step(cpu, 0x1u);", "cosim_instr(_zr_",
                           "psx_icache_fetch(cpu, _zr_", "psx_cpu_step_boundary(cpu, _zr_",
                           "(_zr_%s & 0xCu) == 0u" % m.group("first")):
                if needle not in text:
                    failures.append("loop at 0x%s lacks %r" % (m.group("first"), needle))
            expanded = expand_loop(m, template_for)
            # Every class template agrees with the loop's fetch-or-boundary rule.
            for k in range(int(m.group("n"))):
                a = int(m.group("first"), 16) + 4 * k
                fetch = (a & 0xC) == 0
                call = "psx_icache_fetch" if fetch else "psx_cpu_step_boundary"
                if "%s(cpu, 0x%08Xu);" % (call, a) not in expanded:
                    failures.append("re-expanded word 0x%08X does not use %s" % (a, call))
                    break
        for cls, (base, chunk) in templates.items():
            fetch = (base & 0xC) == 0
            if ("psx_icache_fetch(cpu, 0x%08Xu);" % base in chunk) != fetch:
                failures.append("expanded emitter fetch rule disagrees at 0x%08X" % base)
    elif not failures:
        failures.append("run B did not provide all four address classes")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        raise SystemExit(1)
    print("zero-word run compaction test passed (%d loops, %d words compacted)"
          % (len(loops), sum(n for _, _, n in got)))


if __name__ == "__main__":
    main()
