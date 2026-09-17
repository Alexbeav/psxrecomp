#!/usr/bin/env python3
"""Recompiler codegen regression test: split pieces never run stale static code.

A game can overwrite code inside its own EXE text at runtime (Colin McRae
Rally 2.0 streams patched transform loops over its boot image; finding
PSX-OVL-003). Dispatch validates an entry's emitted ranges against live RAM,
but a root seeded inside a function (call_root / dispatch_root) caps the host
and registers the suffix as an independent piece. Every edge from one piece
into another must then pass the same validation before native code runs:

  * direct host calls (straight-line fallthrough into a split piece, a body
    running off its end into the next function, and the legacy non-CPS
    branch/jump/jal/jalr continuation transfers) carry the emitted
    stale-static guard for the exact target, and
  * CPS branch/jump/jal/jalr edges publish the PC and re-enter through the
    validating dispatch table.

The test synthesizes an EXE whose capped suffixes are reached by each edge
shape, including a transitive F -> M1 -> M2 fallthrough chain and delay slots
that are the first word of a suffix. It then:

  1. audits every direct host call in CPS and legacy output for a guard on
     the same target;
  2. compiles the real emitted CPS bodies and dispatch table, mutates one
     word in each downstream piece, and requires execution to stop at the
     first stale piece (the interpreter handoff) with no stale instruction
     executed;
  3. repeats step 2 with the guards stripped (the pre-guard emitter) and
     requires the harness to catch the stale fallthroughs, so a harness that
     cannot see the bug fails the test.

The range predicate is a byte-exact stand-in for
dirty_ram_text_native_ok_ranges_from (runtime/src/memory.c); its page-cache
fast path and full-range semantics are covered by the runtime tests.

Usage:  python test_split_piece_handoff_guards.py [--recompiler <psxrecomp-game>]
            [--compiler <cc>]
Exit 0 = PASS.
"""
import argparse, os, pathlib, re, shutil, struct, subprocess, sys, tempfile

LOAD = 0x80010000
SIZE = 0x1000
SENTINEL = 0xBEEF0000


def jal(t): return 0x0C000000 | ((t >> 2) & 0x03FFFFFF)
def j(t): return 0x08000000 | ((t >> 2) & 0x03FFFFFF)
def beq(rs, rt, frm, to): return 0x10000000 | (rs << 21) | (rt << 16) | (((to - frm - 4) >> 2) & 0xFFFF)
def bne(rs, rt, frm, to): return 0x14000000 | (rs << 21) | (rt << 16) | (((to - frm - 4) >> 2) & 0xFFFF)
def addiu(rt, rs, imm): return 0x24000000 | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def lui(rt, imm): return 0x3C000000 | (rt << 16) | (imm & 0xFFFF)
def ori(rt, rs, imm): return 0x34000000 | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def jalr(rs): return (rs << 21) | (31 << 11) | 0x09


NOP, JR_RA = 0x00000000, 0x03E00008
PRO, EPI = addiu(29, 29, -16), addiu(29, 29, 16)
SAVE_RA = (31 << 21) | (16 << 11) | 0x21   # addu s0, ra, zero
LOAD_RA = (16 << 21) | (31 << 11) | 0x21   # addu ra, s0, zero


def build_fixture():
    """Return (exe bytes, capping seeds). v0 accumulates a per-instruction
    signature, so the final v0 proves which instructions ran natively."""
    words, roots = {}, []

    def put(off, ws):
        for i, x in enumerate(ws):
            words[LOAD + off + 4 * i] = x

    entry = []
    for s in (0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800):
        entry += [jal(LOAD + s), NOP]
    put(0, entry + [j(LOAD), NOP])
    # Transitive straight-line fallthrough F -> M1 -> M2.
    put(0x100, [PRO, addiu(2, 0, 1), addiu(2, 2, 1),
                addiu(2, 2, 2), addiu(2, 2, 2),            # M1 0x10C
                addiu(2, 2, 3), JR_RA, EPI])               # M2 0x114
    roots += [("call_root", 0x10C), ("call_root", 0x114)]
    # Conditional branch over the cap; not-taken falls into the suffix.
    put(0x200, [PRO, beq(4, 0, LOAD + 0x204, LOAD + 0x218), NOP, addiu(2, 0, 1),
                addiu(2, 2, 1),                            # M 0x210
                addiu(2, 2, 1), addiu(2, 2, 7), JR_RA, EPI])
    roots += [("call_root", 0x210)]
    # Jump past the cap; the suffix branches backward into the host.
    put(0x300, [PRO, addiu(2, 0, 1), j(LOAD + 0x31C), NOP,
                addiu(2, 2, 1), bne(2, 0, LOAD + 0x314, LOAD + 0x304), NOP,  # M 0x310
                addiu(2, 2, 5), JR_RA, EPI])
    roots += [("dispatch_root", 0x310)]
    # jal whose return continuation is the suffix.
    put(0x400, [PRO, SAVE_RA, jal(LOAD + 0x900), NOP,
                addiu(2, 2, 1), LOAD_RA, JR_RA, EPI])      # M 0x410
    roots += [("call_root", 0x410)]
    # jalr whose return continuation is the suffix.
    put(0x500, [PRO, SAVE_RA, lui(8, LOAD >> 16), ori(8, 8, 0x0900), jalr(8), NOP,
                addiu(2, 2, 1), LOAD_RA, JR_RA, EPI])      # M 0x518
    roots += [("dispatch_root", 0x518)]
    # Branch whose delay slot is the suffix's first word.
    put(0x600, [PRO, addiu(2, 0, 1), beq(4, 0, LOAD + 0x608, LOAD + 0x61C),
                addiu(2, 2, 9),                            # M 0x60C
                addiu(2, 2, 1), JR_RA, EPI, NOP, addiu(2, 2, 2), JR_RA, EPI])
    roots += [("call_root", 0x60C)]
    # Jump whose delay slot is the suffix's first word.
    put(0x700, [PRO, addiu(2, 0, 1), j(LOAD + 0x714), addiu(2, 2, 1),  # M 0x70C
                addiu(2, 2, 100), JR_RA, EPI])
    roots += [("dispatch_root", 0x70C)]
    # Body running off its end into the next registered function.
    put(0x800, [PRO, addiu(2, 0, 1), EPI])
    put(0x80C, [addiu(2, 2, 1), JR_RA, NOP])
    roots += [("call_root", 0x80C)]
    put(0x900, [JR_RA, NOP])

    text = bytearray(SIZE)
    for addr, word in words.items():
        struct.pack_into("<I", text, addr - LOAD, word)
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD)
    struct.pack_into("<I", header, 0x18, LOAD)
    struct.pack_into("<I", header, 0x1C, SIZE)
    struct.pack_into("<I", header, 0x30, 0x801FFFF0)
    return bytes(header + text), roots


# (entry offset, $a0, mutated word offset or None, expected stop pc, expected v0)
CASES = [
    (0x100, 0, None, SENTINEL, 9), (0x100, 0, 0x10C, 0x10C, 2), (0x100, 0, 0x114, 0x114, 6),
    (0x200, 0, None, SENTINEL, 7), (0x200, 0, 0x218, 0x218, 0),
    (0x200, 1, None, SENTINEL, 10), (0x200, 1, 0x210, 0x210, 1), (0x200, 1, 0x218, 0x218, 3),
    (0x300, 0, None, SENTINEL, 6), (0x300, 0, 0x31C, 0x31C, 1),
    (0x400, 0, None, SENTINEL, 1), (0x400, 0, 0x410, 0x410, 0),
    (0x500, 0, None, SENTINEL, 1), (0x500, 0, 0x518, 0x518, 0),
    (0x600, 0, None, SENTINEL, 12), (0x600, 0, 0x60C, 0x600, 0), (0x600, 0, 0x620, 0x61C, 10),
    (0x700, 0, None, SENTINEL, 2), (0x700, 0, 0x70C, 0x700, 0),
    (0x800, 0, None, SENTINEL, 2), (0x800, 0, 0x80C, 0x80C, 1),
]

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "psx_runtime.h"
#define LOAD @LOAD@u
#define SIZE @SIZE@u
static uint8_t live[SIZE], ref[SIZE];
/* Byte-exact stand-in for memory.c's emitted-range predicate. */
int dirty_ram_text_native_ok_ranges_from(const uint32_t* p, uint32_t n, uint32_t exec_pc) {
    (void)exec_pc;
    if (!p || !n) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = (p[2 * i] & 0x1FFFFFFFu) - (LOAD & 0x1FFFFFFFu), len = p[2 * i + 1];
        if (off >= SIZE || len > SIZE - off || memcmp(live + off, ref + off, len)) return 0;
    }
    return 1;
}
int dirty_ram_text_native_ok_ranges(const uint32_t* p, uint32_t n) {
    return dirty_ram_text_native_ok_ranges_from(p, n, 0);
}
int g_psx_cps_mode = 1;
volatile uint32_t g_psx_last_fn_entry;
void psx_check_interrupts_at(CPUState* c, uint32_t pc) { (void)c; (void)pc; }
void psx_check_interrupts_dispatch_entry(CPUState* c, uint32_t pc) { (void)c; (void)pc; }
int psx_vsync_query_hle_try(CPUState* c, uint32_t a) { (void)c; (void)a; return 0; }
extern int psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr);
static const struct { uint32_t entry, a0; int mut; uint32_t stop, v0; } cases[] = { @CASES@ };
int main(int argc, char** argv) {
    FILE* f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f || fseek(f, 2048, SEEK_SET) || fread(ref, 1, SIZE, f) != SIZE) return 2;
    fclose(f);
    int fails = 0;
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        memcpy(live, ref, SIZE);
        if (cases[k].mut >= 0) live[cases[k].mut] ^= 0x40;  /* one addiu immediate */
        CPUState cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.gpr[4] = cases[k].a0;
        cpu.gpr[29] = 0x801FFF00u;
        cpu.gpr[31] = @SENTINEL@u;
        uint32_t pc = LOAD + cases[k].entry;
        for (int hops = 0;; hops++) {
            if (hops == 64) { pc = 0xFFFFFFFFu; break; }
            cpu.pc = 0;
            if (!psx_dispatch_game_compiled(&cpu, pc)) break;  /* interpreter handoff */
            pc = cpu.pc;
        }
        int ok = pc == cases[k].stop && cpu.gpr[2] == cases[k].v0;
        fails += !ok;
        printf("  %s entry=%08X a0=%u mutated=%08X stop=%08X (want %08X) v0=%u (want %u)\n",
               ok ? "ok  " : "FAIL", LOAD + cases[k].entry, cases[k].a0,
               cases[k].mut < 0 ? 0u : LOAD + (uint32_t)cases[k].mut,
               pc, cases[k].stop, cpu.gpr[2], cases[k].v0);
    }
    return fails != 0;
}
"""

GUARD = re.compile(r"psx_game_text_native_ok\(0x([0-9A-F]{8})u\)")
DIRECT_CALL = re.compile(r"\bfunc_([0-9A-F]{8})\(cpu\)")
GUARD_LINE = "/* stale-static guard */"


def recompile(recompiler, root, exe, roots, cps):
    (root / "generated").mkdir(parents=True)
    (root / "test.exe").write_bytes(exe)
    (root / "seeds.txt").write_text(
        "0x%08X\n" % LOAD + "".join("%s 0x%08X\n" % (k, LOAD + o) for k, o in roots))
    (root / "game.toml").write_text(f'''[game]
name = "Split-piece handoff guard test"
exe = "test.exe"
load_address = "0x{LOAD:08X}"
entry_pc = "0x{LOAD:08X}"
text_size = "0x{SIZE:X}"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
discovery = "reachable"
''')
    env = dict(os.environ)
    env["PSX_CPS"] = "1" if cps else "0"
    r = subprocess.run([recompiler, "--config", str(root / "game.toml")],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", env=env)
    if r.returncode != 0:
        raise SystemExit("recompiler failed (%s):\n%s" % ("CPS" if cps else "legacy",
                                                          r.stdout + r.stderr))
    shards = sorted((root / "generated").glob("test.exe_full*.c"))
    if not shards:
        raise SystemExit("no _full*.c shard emitted")
    return shards


def unguarded_direct_calls(shards):
    """Direct host calls to a compiled body lacking a guard for that target on
    the same line or the line before. Alias wrappers enter their own host body
    (psx_alias_body_*) for the entry dispatch already validated."""
    bad, count = [], 0
    for shard in shards:
        lines = shard.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped.startswith(("void ", "extern ", "/*")):
                continue
            for call in DIRECT_CALL.finditer(line):
                count += 1
                guarded = set(GUARD.findall(line))
                if i:
                    guarded |= set(GUARD.findall(lines[i - 1]))
                if call.group(1) not in guarded:
                    bad.append("%s:%d: %s" % (shard.name, i + 1, stripped))
    return count, bad


def run_harness(compiler, root, full_source, name, runtime_include):
    work = root / name
    work.mkdir()
    full = work / "full.c"
    full.write_text(full_source, encoding="utf-8")
    cases = ", ".join("{0x%X, %d, %d, 0x%08Xu, %du}" % (
        entry, a0, -1 if mut is None else mut,
        stop if stop == SENTINEL else LOAD + stop, v0)
        for entry, a0, mut, stop, v0 in CASES)
    harness = (HARNESS.replace("@LOAD@", "0x%08X" % LOAD).replace("@SIZE@", "0x%X" % SIZE)
               .replace("@SENTINEL@", "0x%08X" % SENTINEL).replace("@CASES@", cases))
    (work / "harness.c").write_text(harness, encoding="utf-8")
    binary = work / ("harness.exe" if os.name == "nt" else "harness")
    subprocess.run([compiler, "-std=gnu11", "-O1", "-DPSX_NO_DEBUG_TOOLS",
                    "-I" + str(root / "generated"), "-I" + str(runtime_include),
                    str(full), str(root / "generated" / "test.exe_dispatch.c"),
                    str(work / "harness.c"), "-o", str(binary)], check=True)
    r = subprocess.run([str(binary), str(root / "test.exe")],
                       capture_output=True, text=True)
    if r.returncode not in (0, 1):
        raise SystemExit("%s harness could not run (rc %d)" % (name, r.returncode))
    return r.returncode == 0, r.stdout


def main():
    here = pathlib.Path(__file__).resolve().parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", default=str(here.parent / "build" / "psxrecomp-game"))
    ap.add_argument("--compiler", default=os.environ.get("CC") or shutil.which("gcc")
                    or shutil.which("cc"))
    args = ap.parse_args()
    recompiler = os.path.abspath(args.recompiler)
    if not os.path.isfile(recompiler) and os.path.isfile(recompiler + ".exe"):
        recompiler += ".exe"
    if not os.path.isfile(recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)
    if not args.compiler or os.path.basename(args.compiler).lower().startswith(("cl", "clang-cl")):
        raise SystemExit("a GNU-compatible C compiler is required (generated code uses GNU C)")
    runtime_include = here.parent.parent / "runtime" / "include"
    exe, roots = build_fixture()
    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for cps in (True, False):
            root = tmp / ("cps" if cps else "legacy")
            shards = recompile(recompiler, root, exe, roots, cps)
            count, bad = unguarded_direct_calls(shards)
            mode = "CPS" if cps else "legacy"
            if count == 0:
                failures.append("%s: fixture produced no direct inter-piece host calls" % mode)
            for entry in bad:
                failures.append("%s: unguarded direct host call %s" % (mode, entry))
            print("%s: %d direct host calls audited" % (mode, count))

        cps_root = tmp / "cps"
        shards = sorted((cps_root / "generated").glob("test.exe_full*.c"))
        full = "\n".join(s.read_text(encoding="utf-8") for s in shards)
        ok, log = run_harness(args.compiler, cps_root, full, "emitted", runtime_include)
        if not ok:
            failures.append("emitted code executed a stale split piece:\n" + log)
        stripped = "\n".join(l for l in full.splitlines() if GUARD_LINE not in l)
        if stripped == full:
            failures.append("emitted CPS code carries no stale-static guards to strip")
        else:
            ok, log = run_harness(args.compiler, cps_root, stripped, "unguarded",
                                  runtime_include)
            if ok:
                failures.append("harness did not detect stale fallthrough with guards "
                                "stripped; it cannot see the bug it guards")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("PASS: every inter-piece host edge validates its target before native entry")
    return 0


if __name__ == "__main__":
    sys.exit(main())
