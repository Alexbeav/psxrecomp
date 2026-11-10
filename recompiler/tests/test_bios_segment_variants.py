#!/usr/bin/env python3
"""BIOS segment variants (docs/SEGMENT_AWARE_CODE.md §5.4, rollout PR D).

The BIOS copies code into RAM and runs it at one segment per copy window (the
OpenBIOS and SCPH-1001 kernels at KUSEG 0x500). Code entered through another
segment's alias of that window runs there on hardware: SCPH-1001's reset code
enters its kernel through the uncached alias 0xA0000500, so each of those
instructions is an uncached fetch. A segment-qualified seed in the BIOS seeds
file (a runtime PC in another segment than its window's) asks for a variant:
the function at those ROM bytes and its direct-edge closure are emitted again
with the seed's segment, and the dispatch runs a variant for its exact PC.

OpenBIOS is bundled, so this test runs the real psxrecomp-bios on it with three
extra seeds (KSEG1 variants of kernel 0x12BC and 0x13F4, and a KSEG0 variant
of kernel memset 0x540) and checks:
  - the home output is unchanged: the variant build only adds lines;
  - 0x12BC's closure also covers 0x6F8, which it calls with jal (the call
    targets the KSEG1 PC 0xA00006F8), and 0x70C, which 0x6F8 falls through
    into; 0x13F4's covers 0x1424, which only its conditional branch at
    0x1408 reaches;
  - every PC a variant bakes is in its segment: fetch tags, store PCs, links,
    unaligned-access PCs, IRQ resume and fallthrough PCs (the kernel runs at
    KUSEG, so no KUSEG PC of the window may appear);
  - a KSEG1 variant charges a fetch before every instruction (§5.6), while a
    KSEG0 variant keeps the cached line-leader rule;
  - a jump-table case whose value is in another segment than the variant is
    not a local case (memset's table holds KUSEG PCs), so it is dispatched;
  - the dispatch gains an exact-PC variant table (entries and continuations,
    sorted) consulted after the normalized row is found. The emitted lookup
    (normalize(), the variant table, psx_bios_key_home_pc() and
    psx_bios_hit_body()) is compiled and queried, with and without variants:
    each function row's home PC is the PC its body was compiled for (its
    first fetch tag), every variant row runs its own body, every home key's
    home PC runs the
    home body silently, and every other segment's alias of a home key runs
    the home body and is recorded as a BIOS segment miss (kind BIOS, with the
    home PC), so a new alias entry is loud;
  - a variant seed that names no emitted function entry stops the build, and
    so does one in a segment that does not map physical memory
    (0x20000000-0x7FFFFFFF, KSEG2); a runtime PC in its window's own segment
    is no variant request and stays a discovery seed;
  - SCPH-1001's seeds ask for its KSEG1 kernel entry 0xA0000500 (the retail
    image is not in the tree, so that part is a static check).

Usage: python test_bios_segment_variants.py --bios-recompiler <psxrecomp-bios>
                                            [--compiler <cc>]
Exit 0 = PASS.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
KSEG0, KSEG1 = 0x80000000, 0xA0000000
VARIANTS = {0xA00012BC: "variant_kseg1_12BC", 0x80000540: "variant_kseg0_540",
            0xA00013F4: "variant_kseg1_13F4"}


def check(cond, msg):
    if not cond:
        raise SystemExit("FAIL: " + msg)


def profile(tmp, seeds_path):
    with open(os.path.join(ROOT, "bios", "OpenBIOS.toml")) as f:
        text = f.read()
    # TOML literal strings, and function replacements: a Windows path's
    # backslashes are neither TOML nor re.sub escapes.
    rom = os.path.join(ROOT, "bios", "openbios.bin")
    text = re.sub(r'^seeds\s*=.*$', lambda m: "seeds = '%s'" % seeds_path, text, flags=re.M)
    text = re.sub(r'^rom\s*=.*$', lambda m: "rom = '%s'" % rom, text, flags=re.M)
    path = os.path.join(tmp, "OpenBIOS_variants.toml")
    with open(path, "w") as f:
        f.write(text)
    return path


def emit(binary, config, out):
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([binary, "--config", config, "--out-dir", out], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return r, None, None
    with open(os.path.join(out, "OpenBIOS_full.c")) as f:
        full = f.read()
    with open(os.path.join(out, "OpenBIOS_dispatch.c")) as f:
        disp = f.read()
    return r, full, disp


def body(full, name):
    m = re.search(r"^void %s\(CPUState\* cpu\) \{\n(.*?)^\}" % name, full, re.M | re.S)
    return m.group(1) if m else None


def emit_with(binary, tmp, name, seeds, extra):
    """Emit OpenBIOS with extra seed addresses; returns (result, full, dispatch)."""
    data = dict(seeds, seeds=seeds["seeds"] + [
        {"address": "0x%08X" % pc, "label": label, "rationale": "test"}
        for pc, label in extra])
    path = os.path.join(tmp, name + ".json")
    with open(path, "w") as f:
        json.dump(data, f)
    return emit(binary, profile(tmp, path), os.path.join(tmp, name))


def fragment(src, begin, end):
    i = src.index(begin)
    return src[i:src.index(end, i) + len(end)]


# The emitted lookup a normalized dispatch hit runs, compiled and queried. The
# fragments are the emitted C itself; only the CPU state, the home body and
# the segment-miss recorder are doubles.
HARNESS_HEAD = r"""
#include <stdint.h>
#include <stdio.h>
typedef struct { uint32_t gpr[32]; uint32_t pc; } CPUState;
#define PSX_SEGMENT_MISS_BIOS 1u
static unsigned records;
static uint32_t rec_addr, rec_home, rec_kind;
static void psx_segment_miss_record_kind(uint32_t addr, uint32_t home, uint32_t ra,
                                         uint32_t sp, uint32_t frame, uint32_t kind) {
    (void)ra; (void)sp; (void)frame;
    records++; rec_addr = addr; rec_home = home; rec_kind = kind;
}
uint64_t s_frame_count;
uint32_t g_psx_ram_mask = 0x001FFFFFu;
static void home_body(CPUState* cpu) { (void)cpu; }
"""

HARNESS_MAIN = r"""
static int failures;
#define FAIL(...) do { if (failures++ < 20) { printf(__VA_ARGS__); printf("\n"); } } while (0)
static int is_home_key(uint32_t k) {
    for (unsigned i = 0; i < N_KEYS; ++i) if (home_keys[i] == k) return 1;
    return 0;
}
static int variant_of(uint32_t pc) {
    for (unsigned i = 0; i < N_VARIANTS; ++i) if (variant_pcs[i] == pc) return (int)i;
    return -1;
}
int main(void) {
    static const uint32_t segs[3] = {0x00000000u, 0x80000000u, 0xA0000000u};
    CPUState cpu = {{0}, 0};
    unsigned vectors = 0, homes = 0, aliases = 0, aliases_recorded = 0, variants_hit = 0;
    for (unsigned i = 0; i < N_VARIANTS; ++i) {
        const uint32_t pc = variant_pcs[i], key = normalize(pc);
        const unsigned before = records;
        if (!is_home_key(key)) FAIL("variant 0x%08X: its word 0x%08X has no home row", pc, key);
        if (psx_bios_hit_body(&cpu, pc, key, home_body) != variant_fns[i] || records != before)
            FAIL("variant 0x%08X does not run its own body silently", pc);
        else variants_hit++;
    }
    for (unsigned i = 0; i < N_KEYS; ++i) {
        const uint32_t key = home_keys[i], home = psx_bios_key_home_pc(key);
        if (home == 0u) {
            if (key != 0xA0u && key != 0xB0u && key != 0xC0u)
                FAIL("key 0x%08X is in no window and is no call vector", key);
            vectors++;
            continue;
        }
        if (normalize(home) != key) FAIL("key 0x%08X: home PC 0x%08X normalizes elsewhere", key, home);
        if (entry_pcs[i] != 0u && entry_pcs[i] != home)
            FAIL("key 0x%08X: home PC 0x%08X, but its body was compiled for 0x%08X",
                 key, home, entry_pcs[i]);
        if (variant_of(home) >= 0) FAIL("home PC 0x%08X has a variant row", home);
        {
            const unsigned before = records;
            if (psx_bios_hit_body(&cpu, home, key, home_body) != home_body || records != before)
                FAIL("home PC 0x%08X does not run its home body silently", home);
            homes++;
        }
        for (unsigned s = 0; s < 3; ++s) {
            const uint32_t alias = segs[s] | (home & 0x1FFFFFFFu);
            const unsigned before = records;
            if (alias == home || normalize(alias) != key) continue;
            aliases++;
            const int v = variant_of(alias);
            PsxRecompFunc got = psx_bios_hit_body(&cpu, alias, key, home_body);
            if (v >= 0) {
                if (got != variant_fns[v] || records != before)
                    FAIL("alias 0x%08X does not run its variant silently", alias);
            } else if (got != home_body || records != before + 1 || rec_addr != alias ||
                       rec_home != home || rec_kind != PSX_SEGMENT_MISS_BIOS) {
                FAIL("alias 0x%08X of home 0x%08X is not a recorded BIOS segment miss", alias, home);
            } else {
                aliases_recorded++;
            }
        }
    }
    printf("%u %u %u %u %u %d\n", variants_hit, homes, aliases, aliases_recorded, vectors, failures);
    return failures != 0;
}
"""


def run_lookup_harness(compiler, disp, full, tmp, name):
    """Compile the emitted hit lookup of `disp` and query it; returns
    (variants hit, home PCs, aliases, aliases recorded, call vectors)."""
    rows = re.search(r"static const DispatchEntry dispatch_table\[\d+\] = \{\n(.*?)\n\};",
                     disp, re.S)
    check(rows, "the dispatch table is emitted")
    keys = re.findall(r"\{ 0x([0-9A-F]{8})u, \w+ \}", rows.group(1))
    # Ground truth for psx_bios_key_home_pc(): a function row's body charges
    # its entry's fetch first, and that tag is the runtime PC it was compiled
    # for.
    entry_pc = {}
    for key, sym in re.findall(r"\{ 0x([0-9A-F]{8})u, (\w+) \}", rows.group(1)):
        text = None if "_cont_" in sym else body(full, sym)
        m = text and re.search(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)", text)
        if m:
            entry_pc[key] = m.group(1)
    check({int(pc, 16) & 0xE0000000 for pc in entry_pc.values()} == {0, KSEG0, KSEG1},
          "function rows in all three windows (kernel, shell, ROM) give their compiled PC")
    vm = re.search(r"static const DispatchEntry segment_variant_table\[\d+\] = \{\n.*?\n\};\n",
                   disp, re.S)
    variants = re.findall(r"\{ 0x([0-9A-F]{8})u, (\w+) \}", vm.group(0)) if vm else []
    src = HARNESS_HEAD
    src += "".join("static void %s(CPUState* cpu) { (void)cpu; }\n" % sym for _, sym in variants)
    src += fragment(disp, "typedef void (*PsxRecompFunc)(CPUState*);", "} DispatchEntry;\n")
    if vm:
        src += vm.group(0)
    src += fragment(disp, "extern uint32_t g_psx_ram_mask;", "    return phys;\n}\n")
    src += fragment(disp, "/* The runtime PC each dispatch key's home body",
                    "    return home;\n}\n")
    src += "static const uint32_t home_keys[] = {%s};\n" % ", ".join("0x%su" % k for k in keys)
    src += "static const uint32_t entry_pcs[] = {%s};\n" % ", ".join(
        "0x%su" % entry_pc.get(k, "00000000") for k in keys)
    src += "#define N_KEYS %du\n#define N_VARIANTS %du\n" % (len(keys), len(variants))
    src += "static const uint32_t variant_pcs[] = {%s};\n" % (
        ", ".join("0x%su" % a for a, _ in variants) or "0u")
    src += "static const PsxRecompFunc variant_fns[] = {%s};\n" % (
        ", ".join(sym for _, sym in variants) or "0")
    src += HARNESS_MAIN
    path = os.path.join(tmp, name + ".c")
    exe = os.path.join(tmp, name + (".exe" if os.name == "nt" else ""))
    with open(path, "w") as f:
        f.write(src)
    if os.path.basename(compiler).lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        cmd = [compiler, "/nologo", "/Od", path, "/Fe:" + exe]
    else:
        cmd = [compiler, "-std=c11", "-O1", path, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=tmp)
    check(r.returncode == 0, "the emitted dispatch lookup compiles:\n" + (r.stdout + r.stderr)[-3000:])
    r = subprocess.run([exe], capture_output=True, text=True)
    check(r.returncode == 0, "the emitted dispatch lookup answers every query:\n" + r.stdout[-3000:])
    return tuple(int(v) for v in r.stdout.split()[:5])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios-recompiler",
                    default=os.path.join(ROOT, "recompiler", "build", "psxrecomp-bios"))
    ap.add_argument("--compiler",
                    default=os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc"))
    args = ap.parse_args()
    if not args.compiler:
        raise SystemExit("a C compiler is required to query the emitted dispatch lookup")
    if not os.path.isfile(args.bios_recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.bios_recompiler)

    with open(os.path.join(ROOT, "recompiler", "seeds", "openbios_elf_seeds.json")) as f:
        seeds = json.load(f)

    with tempfile.TemporaryDirectory() as tmp:
        base_seeds = os.path.join(tmp, "base.json")
        with open(base_seeds, "w") as f:
            json.dump(seeds, f)
        r, base_full, base_disp = emit(args.bios_recompiler, profile(tmp, base_seeds),
                                       os.path.join(tmp, "base"))
        check(base_full is not None, "baseline OpenBIOS emission failed:\n" + r.stderr[-2000:])

        var = dict(seeds, seeds=seeds["seeds"] + [
            {"address": "0x%08X" % pc, "label": label, "rationale": "test"}
            for pc, label in VARIANTS.items()])
        var_seeds = os.path.join(tmp, "variants.json")
        with open(var_seeds, "w") as f:
            json.dump(var, f)
        r, full, disp = emit(args.bios_recompiler, profile(tmp, var_seeds),
                             os.path.join(tmp, "var"))
        check(full is not None, "OpenBIOS with variant seeds failed:\n" + r.stderr[-2000:])
        for pc, label in VARIANTS.items():
            check("segment-variant seed 0x%08X (%s)" % (pc, label) in r.stdout,
                  "the variant seed 0x%08X is reported" % pc)

        # -- the home output only gains lines -------------------------------
        def only_additions(old, new):
            it = iter(new.splitlines())
            return all(any(line == n for n in it) for line in old.splitlines())
        check(only_additions(base_full, full), "the variant build changes no home body")
        check(only_additions(base_disp, disp), "the variant build changes no home dispatch line")

        # -- closure and segment of every baked PC ---------------------------
        names = set(re.findall(r"^void (OpenBIOS_func_[0-9A-F]{8})\(CPUState\* cpu\) \{",
                               full, re.M))
        want = {"OpenBIOS_func_A00012BC", "OpenBIOS_func_A00006F8", "OpenBIOS_func_A000070C",
                "OpenBIOS_func_80000540", "OpenBIOS_func_A00013F4", "OpenBIOS_func_A0001424"}
        check(want <= names, "variant bodies, 0x6F8 by jal, 0x70C by fall-through and 0x1424 "
              "by a conditional branch: %s" % sorted(n for n in names if n[14] in "8A"))
        check("cpu->pc = 0xA00006F8u; return;" in body(full, "OpenBIOS_func_A00012BC"),
              "a KSEG1 variant's jal keeps the KSEG1 top bits")
        pcs = r"(?:psx_icache_fetch\(cpu, |g_debug_last_store_pc = |psx_unaligned_access" \
              r"\(cpu, psx_addr, |psx_check_interrupts_at\(cpu, |cpu->gpr\[\d+\] = |" \
              r"cpu->pc = )0x([0-9A-F]{8})u"
        for name in sorted(want):
            seg = int(name[14:], 16) & 0xE0000000
            text = body(full, name)
            check(text, "no body for " + name)
            baked = [int(v, 16) for v in re.findall(pcs, text)]
            window = [v for v in baked if 0x500 <= (v & 0x1FFFFFFF) < 0x8000]
            check(window and all((v & 0xE0000000) == seg for v in window),
                  "%s bakes only its segment's kernel PCs, got %s" %
                  (name, sorted({"%08X" % v for v in window if (v & 0xE0000000) != seg})))
            insns = re.findall(r"/\* 0x([0-9A-F]{8}): [0-9A-F]{8} ", text)
            fetches = re.findall(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)", text)
            if seg == KSEG1:
                check(len(fetches) == len(insns),
                      "%s charges a fetch per instruction (%d fetches, %d instructions)"
                      % (name, len(fetches), len(insns)))
            else:
                check(0 < len(fetches) < len(insns),
                      "%s keeps the cached line-leader rule" % name)
        memset = body(full, "OpenBIOS_func_80000540")
        home_memset = body(full, "OpenBIOS_func_00000540")
        check(re.search(r"case 0x000005[0-9A-F]{2}u:", home_memset) and
              "jump table" in home_memset,
              "the home memset switches locally on its KUSEG table values")
        check(not re.search(r"case 0x000005[0-9A-F]{2}u:", memset) and
              re.search(r"cpu->pc = (?:psx_jrt_BFC1E574|cpu->gpr\[9\]); return;", memset),
              "the KSEG0 memset variant dispatches the KUSEG case values")

        # -- dispatch -------------------------------------------------------
        m = re.search(r"segment_variant_table\[(\d+)\] = \{\n(.*?)\n\};", disp, re.S)
        check(m, "the dispatch has a segment-variant table")
        rows = [(int(a, 16), sym) for a, sym in
                re.findall(r"\{ 0x([0-9A-F]{8})u, (\w+) \}", m.group(2))]
        check(int(m.group(1)) == len(rows) and [a for a, _ in rows] == sorted(a for a, _ in rows),
              "the variant rows are counted and sorted")
        keys = {a: sym for a, sym in rows}
        check(keys.get(0xA00012BC) == "OpenBIOS_func_A00012BC" and
              keys.get(0xA00006F8) == "OpenBIOS_func_A00006F8" and
              keys.get(0xA000070C) == "OpenBIOS_func_A000070C" and
              keys.get(0x80000540) == "OpenBIOS_func_80000540",
              "each variant entry is keyed by its exact runtime PC")
        check(any(sym.startswith("OpenBIOS_func_A000070C_cont_") and (a & 0xE0000000) == KSEG1
                  for a, sym in rows),
              "a variant's call returns are exact-PC continuation rows")
        check("psx_bios_hit_body(cpu, addr, phys, dispatch_table[mid].func)(cpu);" in disp and
              "psx_bios_hit_body(cpu, addr, phys, dispatch_table[mid].func)(cpu);" in base_disp,
              "a normalized hit runs the body psx_bios_hit_body picks for the exact PC")
        got = run_lookup_harness(args.compiler, disp, full, tmp, "lookup_var")
        # Every key has a home PC; the A0/B0/C0 vector handlers, when a
        # profile emits them into the table, are the only keys in no window.
        check(got[0] == len(rows) and got[1] > 1000 and got[3] > 1000 and got[4] in (0, 3),
              "the variant lookup: %d of %d variant rows, %d home PCs, %d/%d aliases "
              "recorded, %d call vectors" % (got[0], len(rows), got[1], got[3], got[2], got[4]))
        base_got = run_lookup_harness(args.compiler, base_disp, base_full, tmp, "lookup_base")
        check(base_got[0] == 0 and base_got[1] == got[1] and base_got[2] == base_got[3] ==
              got[2] and got[3] == got[2] - len(rows),
              "without variants every alias of a home key is a recorded BIOS segment miss; "
              "with them, exactly the variant rows are not: base %s, variants %s"
              % (base_got, got))

        # -- a variant seed must name a function entry -----------------------
        r, _, _ = emit_with(args.bios_recompiler, tmp, "bad", seeds, [(0xA0000504, "inside")])
        check(r.returncode != 0 and "segment-variant seed 0xA0000504" in r.stderr,
              "a variant seed inside a function body stops the build:\n" + r.stderr[-2000:])

        # -- only KUSEG below 0x20000000, KSEG0 and KSEG1 map the window ------
        for pc in (0x200012BC, 0xC00012BC):
            r, _, _ = emit_with(args.bios_recompiler, tmp, "badseg", seeds, [(pc, "badseg")])
            check(r.returncode != 0 and
                  ("seed 0x%08X (badseg) is in segment 0x%08X, which does not map physical "
                   "memory" % (pc, pc & 0xE0000000)) in r.stderr and
                  "runs its bytes at 0x000012BC" in r.stderr,
                  "a seed in a segment that does not map physical memory stops the build:\n"
                  + r.stderr[-2000:])

        # -- a runtime PC in its window's own segment is no variant request ---
        # It stays a discovery seed, and discovery takes ROM addresses only.
        r, _, _ = emit_with(args.bios_recompiler, tmp, "same", seeds, [(0x000012BC, "same")])
        check(r.returncode != 0 and "segment-variant seed 0x000012BC" not in r.stdout and
              "seed address 0x000012BC (same) is outside ROM range" in r.stderr,
              "a seed in its window's own segment stays a discovery seed:\n"
              + (r.stdout + r.stderr)[-2000:])

    # -- SCPH-1001 enters its kernel at 0xA0000500 (static: no retail image) ---
    with open(os.path.join(ROOT, "bios", "SCPH1001.toml")) as f:
        m = re.search(r'^seeds\s*=\s*"([^"]+)"', f.read(), re.M)
    with open(os.path.join(ROOT, m.group(1))) as f:
        retail = json.load(f)
    check(any(int(s["address"], 16) == 0xA0000500 for s in retail["seeds"]),
          "SCPH-1001's seeds ask for the KSEG1 variant of its kernel entry 0xA0000500")

    print("PASS: BIOS segment variants (closure, per-segment PCs, KSEG1 per-instruction "
          "fetch, foreign jump-table cases dispatched, exact-PC dispatch rows compiled and "
          "queried, aliases without a variant recorded, seeds outside the physical segments "
          "refused, home output unchanged)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
