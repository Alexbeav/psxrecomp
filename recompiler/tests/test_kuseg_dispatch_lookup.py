#!/usr/bin/env python3
"""Recompiler codegen regression test: a KUSEG-linked EXE compiles for KUSEG,
and dispatch is exact (docs/SEGMENT_AWARE_CODE.md §5.3, §5.5).

PS1 segments alias the same physical RAM, but a PC carries its segment into
every link value, EPC and I-cache tag. Most PS-X EXE headers carry KSEG0
addresses (0x8001xxxx); some carry KUSEG ones (0x0001xxxx: Alien Resurrection
SLUS-00633, Kula World SCES-01000), and the program then runs at KUSEG PCs.

The recompiler used to fold such a header to KSEG0 and look PCs up by physical
address, so every KUSEG PC ran a body that baked KSEG0 links and tags, and a
KSEG0 or KSEG1 alias ran it too. Now:
  - the EXE parser keeps the header's segment (the link segment) and the
    image compiles there: rows, names and the PCs a body bakes are KUSEG;
  - the emitted lookup indexes the physical word and then requires the exact
    PC, so an alias PC with no body of its own misses; psx_game_address_in_text
    stays physical, which sends that segment miss to the interpreter;
  - a seed in the link segment is an entry; a seed for the same bytes in
    another segment is reported as a variant request (§5.4), never folded into
    the home body and never silently dropped;
  - a header whose entry and load address are in different segments is
    rejected, a KSEG1-linked image compiles as uncached (a fetch charge per
    instruction), and a config code site in a foreign segment is refused.

The RAM-mirror contract is pinned against the runtime's real geometry
(runtime/src/psx_ram_geometry.c, linked in): a PC in the 2nd-4th RAM mirror is
a different code address, in retail 2 MiB and 8 MiB geometry alike, so it never
resolves to a compiled body and never counts as EXE text.

Usage:  python test_kuseg_dispatch_lookup.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile

LOAD = 0x00010000          # KUSEG: no KSEG bit, the case under test
PHYS_MASK = 0x1FFFFFFF
SEG_MASK = 0xE0000000
KUSEG, KSEG0, KSEG1 = 0x00000000, 0x80000000, 0xA0000000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)   # stack_base, as real headers carry
    return bytes(h) + data


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def build_body(load):
    # func A @ load: addiu sp,-8 ; sw ra,4(sp) ; jal B ; nop ; lw ra,4(sp) ;
    #                addiu sp,8 ; jr ra ; nop
    # func B @ load+0x20: jr ra ; nop
    a = [0x27BDFFF8, 0xAFBF0004, jal(load + 0x20), 0x00000000, 0x8FBF0004,
         0x27BD0008, 0x03E00008, 0x00000000]
    body = bytearray(w(a))
    body += b"\x00" * (0x20 - len(body))
    body += w([0x03E00008, 0x00000000])
    return bytes(body)


def build_jal_body():
    """Functions only a JAL scan finds, in a KUSEG image (§5.3).

    +00 main: jal B, then jal save with $a0 = $sp; +18 is the return from
        save, a SaveState-style continuation (save stores $ra through $a0).
    +28 a `j .` spin, so nothing before B ends in `jr $ra`.
    +30 B: reached only by main's jal.
    +3C save: sw $ra, 0($a0).
    """
    return w([0x27BDFFF8, 0xAFBF0004, jal(LOAD + 0x30), 0x00000000,
              jal(LOAD + 0x3C), 0x03A02025, 0x8FBF0004, 0x27BD0008,
              0x03E00008, 0x00000000,
              0x0800400A, 0x00000000,
              0x24020001, 0x03E00008, 0x00000000,
              0xAC9F0000, 0x03E00008, 0x00000000])


WIDE_SPAN = 0x200000       # the emitter indexes spans below this; wider ones bsearch


def build_wide_body():
    """Two leaf functions WIDE_SPAN apart: the emitted lookup must take its
    binary-search form (tables that span 2 MiB or more, e.g. 8 MiB titles)."""
    body = bytearray(WIDE_SPAN + 0x800)
    body[0:8] = w([0x03E00008, 0x00000000])
    body[WIDE_SPAN:WIDE_SPAN + 8] = w([0x03E00008, 0x00000000])
    return bytes(body)


def lookup_fragment(src):
    """psx_game_address_in_text .. psx_game_is_function_entry, and the func_
    names it references (bound to a fixture double by the harness)."""
    begin = src.index("int psx_game_address_in_text(uint32_t addr) {")
    entry_probe = src.index("int psx_game_is_function_entry(uint32_t addr) {", begin)
    end = src.index("}\n", entry_probe) + 2
    fragment = src[begin:end]
    return fragment, sorted(set(re.findall(r"func_[0-9A-Fa-f]{8}", fragment)))


def compile_and_run(compiler, harness, sources, include):
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "lookup.c")
        binary = os.path.join(tmp, "lookup.exe" if os.name == "nt" else "lookup")
        with open(source, "w", encoding="utf-8") as f:
            f.write(harness)
        compiler_name = os.path.basename(compiler).lower()
        if compiler_name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [compiler, "/nologo", "/Od", "/I" + include, source] + sources + \
                      ["/Fe:" + binary]
        else:
            command = [compiler, "-std=c11", "-O2", "-I", include, source] + sources + \
                      ["-o", binary]
        subprocess.run(command, check=True, cwd=tmp)
        subprocess.run([binary], check=True, cwd=tmp)


def run(recompiler, args, cwd=ROOT):
    return subprocess.run([recompiler] + args, capture_output=True, text=True, cwd=cwd)


def generate(recompiler, tmp, name, entry, load, seeds, body=None):
    psx = os.path.join(tmp, name + ".psx")
    seed_path = os.path.join(tmp, name + ".seeds.txt")
    out = os.path.join(tmp, name)
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(make_psxexe(entry, load, body if body is not None else build_body(load)))
    with open(seed_path, "w") as f:
        f.write("".join("0x%08X\n" % s for s in seeds))
    r = run(recompiler, [psx, "--seeds", seed_path, "--out-dir", out])
    if r.returncode != 0:
        raise SystemExit("recompiler failed on %s:\n%s" % (name, r.stderr or r.stdout))
    disp = [f for f in os.listdir(out) if f.endswith("_dispatch.c")]
    if not disp:
        raise SystemExit("no _dispatch.c emitted in " + out)
    with open(os.path.join(out, disp[0])) as f:
        dsrc = f.read()
    full = ""
    for n in sorted(os.listdir(out)):
        if re.match(r".*_full(_\d+)?\.c$", n):
            with open(os.path.join(out, n)) as f:
                full += f.read()
    return dsrc, full, r.stdout


def rows_of(src):
    tbl = re.search(r"k_psx_game_dispatch\[\] = \{(.*?)\n\};", src, re.DOTALL)
    if not tbl:
        raise SystemExit("dispatch table not found in emitted dispatch")
    return [(int(a, 16), int(r, 16), fn) for a, r, fn in re.findall(
        r"\{0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, \d+u, \d+u, (func_[0-9A-F]{8})\}", tbl.group(1))]


def check(cond, msg):
    if not cond:
        raise SystemExit("FAIL: " + msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(HERE, "..", "build",
                                                          "psxrecomp-game")))
    ap.add_argument("--compiler", default=os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc"))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        # -- the KUSEG image: seeds in the link segment, plus one KSEG0 seed ---
        src, full, log = generate(args.recompiler, tmp, "kuseg", LOAD, LOAD,
                                  [LOAD, LOAD + 0x20, KSEG0 | (LOAD + 0x20)])
        check("Loaded 2 extra function addresses" in log,
              "KUSEG seeds in the link segment must be loaded (log: %r)"
              % re.findall(r"Loaded .*", log))
        check("variant seed 0x80010020 (entry, KSEG0): home PC would be 0x00010020" in log,
              "a KSEG0 seed for a KUSEG image must be reported as a variant request")
        rows = rows_of(src)
        check(len(rows) >= 2, "expected at least two dispatch entries, got %d" % len(rows))
        check(all((a & SEG_MASK) == KUSEG and (r == 0 or (r & SEG_MASK) == KUSEG)
                  for a, r, _ in rows),
              "every row key and resume PC is a KUSEG PC: %s" % rows)
        check(all((int(fn[5:], 16) & SEG_MASK) == KUSEG and (r != 0 or fn == "func_%08X" % a)
                  for a, r, fn in rows),
              "func_ names are the KUSEG identity: %s" % rows)
        check({a for a, r, _ in rows} >= {LOAD, LOAD + 0x20}, "both functions have rows")
        check("void func_00010000(CPUState* cpu)" in full and
              "void func_00010020(CPUState* cpu)" in full,
              "bodies are named by their KUSEG PC (§5.1)")
        check("cpu->gpr[31] = 0x00010010u;" in full, "the jal links a KUSEG PC")
        check("psx_icache_fetch(cpu, 0x00010000u)" in full, "fetch tags are KUSEG")
        check(not re.search(r"0x8001[0-9A-F]{4}u", full),
              "no KSEG0 PC of the image appears in the KUSEG body")
        keys = [a & PHYS_MASK for a, _, _ in rows]
        check(keys == sorted(keys), "dispatch table is sorted by physical address")

        m = re.search(r"static const PsxGameDispatchEntry\* psx_game_find_entry"
                      r"\(uint32_t addr\) \{(.*?)\n\}", src, re.DOTALL)
        check(m, "psx_game_find_entry not found in emitted dispatch")
        body = m.group(1)
        check("0x1FFFFFFFu" in body and "k_psx_game_dispatch_index" in body,
              "small resident tables index the physical word")
        check(".addr != addr" in body, "the lookup requires the exact PC")

        # -- a table spanning 2 MiB or more takes the binary-search form ------
        wide_src, _, _ = generate(args.recompiler, tmp, "kuseg_wide", LOAD, LOAD,
                                  [LOAD, LOAD + WIDE_SPAN], body=build_wide_body())
        wide_rows = rows_of(wide_src)
        check({LOAD, LOAD + WIDE_SPAN} <= {a for a, _, _ in wide_rows} and
              all((a & SEG_MASK) == KUSEG for a, _, _ in wide_rows),
              "the wide KUSEG image has KUSEG rows at both ends: %s" % wide_rows)
        wm = re.search(r"static const PsxGameDispatchEntry\* psx_game_find_entry"
                       r"\(uint32_t addr\) \{(.*?)\n\}", wide_src, re.DOTALL)
        check(wm and "k_psx_game_dispatch_index" not in wm.group(1) and
              "while (lo < hi)" in wm.group(1),
              "a table spanning 2 MiB or more binary-searches the physical word")

        # -- JAL-driven discovery takes the call's segment, not KSEG0 --------
        jsrc, _, jlog = generate(args.recompiler, tmp, "kuseg_jal", LOAD, LOAD, [],
                                 body=build_jal_body())
        jrows = {a: (r, fn) for a, r, fn in rows_of(jsrc)}
        check(jrows.get(LOAD + 0x30) == (0, "func_00010030"),
              "a function reached only by jal is discovered in a KUSEG image: %s" % jrows)
        check(jrows.get(LOAD + 0x18) == (0, "func_00010018"),
              "the SaveState-style continuation after jal save is an entry: %s" % jrows)
        check("Identified 1 SaveState continuation entries" in jlog,
              "the SaveState scan follows the KUSEG jal")

        # -- a header whose entry is in another segment than its load address
        psx = os.path.join(tmp, "mixed.psx")
        with open(psx, "wb") as f:
            f.write(make_psxexe(KSEG0 | LOAD, LOAD, build_body(LOAD)))
        r = run(args.recompiler, [psx, "--out-dir", os.path.join(tmp, "mixed")])
        check(r.returncode != 0 and "different segments" in (r.stdout + r.stderr),
              "an entry and load address in different segments must be rejected")

        # -- a KSEG1-linked image: uncached home segment -----------------------
        _, k1_full, _ = generate(args.recompiler, tmp, "kseg1", KSEG1 | LOAD, KSEG1 | LOAD,
                                 [KSEG1 | LOAD, KSEG1 | (LOAD + 0x20)])
        body_a = re.search(r"void func_A0010000\(CPUState\* cpu\)\s*\{(.*?)^\}", k1_full,
                           re.M | re.S)
        check(body_a, "the KSEG1 image's entry is named func_A0010000")
        insns = re.findall(r"/\* 0x([0-9A-F]{8}): ", body_a.group(1))
        fetched = set(re.findall(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)", body_a.group(1)))
        check(insns and all(i in fetched for i in insns),
              "every KSEG1 instruction is charged its own fetch (§5.6)")

        # -- config code sites are code identities in the link segment ---------
        exe = os.path.join(tmp, "kuseg.psx")
        for site, ok in ((KSEG0 | (LOAD + 0x20), False), (LOAD + 0x20, True)):
            cfg = os.path.join(tmp, "game_%08X.toml" % site)
            # TOML literal strings: a Windows path's backslashes are not escapes.
            with open(cfg, "w") as f:
                f.write("[game]\nname = 't'\nexe = '%s'\ntext_size = '0x%X'\n"
                        "[recompiler]\nseeds = '%s'\nout_dir = '%s'\n"
                        "hot_funcs = ['0x%08X']\n"
                        % (exe, len(build_body(LOAD)), os.path.join(tmp, "kuseg.seeds.txt"),
                           os.path.join(tmp, "cfg_%08X" % site), site))
            r = run(args.recompiler, ["--config", cfg, "--project-root", ROOT])
            if ok:
                check(r.returncode == 0, "a link-segment config site is accepted:\n%s"
                      % (r.stderr or r.stdout)[-2000:])
            else:
                check(r.returncode != 0 and
                      "recompiler.hot_funcs: 0x80010020 -> 0x00010020" in r.stderr,
                      "a KSEG0 config site for a KUSEG image must be refused:\n%s"
                      % (r.stderr or r.stdout)[-2000:])

        # load_accel return PCs are compared exactly with $ra by the runtime.
        cfg = os.path.join(tmp, "game_horizon.toml")
        with open(cfg, "w") as f:
            f.write("[game]\nname = 't'\nexe = '%s'\ntext_size = '0x%X'\n"
                    "[recompiler]\nseeds = '%s'\nout_dir = '%s'\n"
                    "[load_accel.vsync_query]\nfunc = '0x%08X'\n"
                    "counter_addr = '0x80090000'\ngpustat_ptr_addr = '0x80090004'\n"
                    "timer1_ptr_addr = '0x80090008'\ntimer1_cache_addr = '0x8009000C'\n"
                    "event_horizon_extra_sites = ['0x%08X']\n"
                    % (exe, len(build_body(LOAD)), os.path.join(tmp, "kuseg.seeds.txt"),
                       os.path.join(tmp, "cfg_horizon"), LOAD, KSEG0 | (LOAD + 0x10)))
        r = run(args.recompiler, ["--config", cfg, "--project-root", ROOT])
        check(r.returncode != 0 and
              "load_accel.vsync_query.event_horizon_extra_sites: 0x80010010 -> 0x00010010"
              in r.stderr,
              "a KSEG0 event-horizon return PC for a KUSEG image must be refused:\n%s"
              % (r.stderr or r.stdout)[-2000:])

        # A kind the emitter matches by physical address (a full-word-guarded
        # substitution, like a byte patch) reaches the same bytes in any
        # spelling, so it is not refused, and it still applies.
        lui_exe = os.path.join(tmp, "kuseg_lui.psx")
        with open(lui_exe, "wb") as f:
            f.write(make_psxexe(LOAD, LOAD, w([0x3C020140, 0x03E00008, 0x00000000])))
        lui_seeds = os.path.join(tmp, "kuseg_lui.seeds.txt")
        with open(lui_seeds, "w") as f:
            f.write("0x%08X\n" % LOAD)
        cfg = os.path.join(tmp, "game_bound.toml")
        with open(cfg, "w") as f:
            f.write("[game]\nname = 't'\nexe = '%s'\ntext_size = '0xC'\n"
                    "[recompiler]\nseeds = '%s'\nout_dir = '%s'\n"
                    "[[widescreen.signed_x_bound]]\naddress = '0x%08X'\n"
                    "expected = '0x3C020140'\n"
                    % (lui_exe, lui_seeds, os.path.join(tmp, "cfg_bound"), KSEG0 | LOAD))
        r = run(args.recompiler, ["--config", cfg, "--project-root", ROOT])
        check(r.returncode == 0, "a physically matched config site is accepted in any "
              "segment:\n%s" % (r.stderr or r.stdout)[-2000:])
        bound_c = ""
        for n in os.listdir(os.path.join(tmp, "cfg_bound")):
            if n.endswith(".c"):
                with open(os.path.join(tmp, "cfg_bound", n)) as f:
                    bound_c += f.read()
        check("psx_ws_player_x_bound((int32_t)0x01400000)" in bound_c,
              "the physically matched signed_x_bound site still applies")

        # Seeds-file directives name code by PC too: a foreign spelling is
        # refused with the link-segment one, not a bare "invalid" message.
        for directive, want in (
                ("producer_range 0x80010000 0x80010028",
                 "write its addresses as: 0x00010000 0x00010028"),
                ("cross_call_allow 0x80010020", "write its addresses as: 0x00010020"),
                ("hosted_interior 0x80010010 0x00010000",
                 "write its addresses as: 0x00010010 0x00010000")):
            seeds = os.path.join(tmp, "directive.seeds.txt")
            with open(seeds, "w") as f:
                f.write("0x%08X\n%s\n" % (LOAD, directive))
            r = run(args.recompiler, [exe, "--seeds", seeds, "--out-dir",
                                      os.path.join(tmp, "directive")])
            check(r.returncode != 0 and want in r.stderr,
                  "a KSEG0 %r for a KUSEG image must be refused with its KUSEG "
                  "spelling:\n%s" % (directive, (r.stderr or r.stdout)[-2000:]))

    # Compile the REAL emitted lookup, validity gates and dispatch function,
    # linked against the runtime's REAL live-geometry state machine
    # (runtime/src/psx_ram_geometry.c + psx_memory.h), so the contracts below
    # are checked against exactly what the runtime links -- no copies. Only
    # peripheral callbacks and generated MIPS bodies are fixture doubles.
    if not args.compiler:
        raise SystemExit("a C compiler is required for the dispatch regression")
    runtime = os.path.join(ROOT, "runtime")
    include = os.path.join(runtime, "include")
    fragment, funcs = lookup_fragment(src)
    harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "psx_memory.h"
int psx_mod_set_main_ram_8mb(int enabled);   /* runtime psx_ram_geometry.c */
typedef struct { uint32_t pc; } CPUState;
static int allowed = 1, calls = 0, irqs = 0;
static uint32_t resumed;
static void dummy(CPUState* cpu) { calls++; resumed = cpu->pc; }
static int dirty_ram_text_native_ok_ranges_from(const uint32_t* r, uint32_t n, uint32_t a) {
    (void)r; (void)n; (void)a; return allowed;
}
static int dirty_ram_text_native_ok_ranges(const uint32_t* r, uint32_t n) {
    return dirty_ram_text_native_ok_ranges_from(r, n, 0);
}
static void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t a) {
    (void)cpu; (void)a; irqs++;
}
int psx_vsync_query_hle_try(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
"""
    harness += "\n".join(f"#define {fn} dummy" for fn in funcs) + "\n" + fragment
    harness += r"""
static void set_geometry(int expanded) {
    psx_ram_reset_size_request();
    if (expanded) psx_mod_set_main_ram_8mb(1);
    psx_ram_apply_size_request();
    assert(memory_get_ram_bytes() == (expanded ? 0x00800000u : 0x00200000u));
}

int main(void) {
    const uint32_t segments[] = {0x00000000u, 0x80000000u, 0xA0000000u};
    CPUState cpu = {0};
    unsigned home_hits = 0;
    /* Retail (8 MB mod off) first, then the expanded map: code identity is the
     * full PC in BOTH, so geometry never changes which compiled body a PC may
     * run. */
    for (int expanded = 0; expanded < 2; ++expanded) {
        set_geometry(expanded);
        for (unsigned s = 0; s < 3; ++s) {
            for (uint32_t phys = 0xFFF0u; phys < 0x10080u; ++phys) {
                uint32_t addr = segments[s] | phys;
                /* Exact: only a row compiled for this very PC answers. The
                 * image is KUSEG-linked, so its KSEG0 and KSEG1 aliases have
                 * no body and miss (a segment miss, interpreted by the
                 * runtime), while text membership stays physical. */
                const PsxGameDispatchEntry* expected = 0;
                for (unsigned i = 0; i < PSX_GAME_DISPATCH_COUNT; ++i)
                    if (k_psx_game_dispatch[i].addr == addr)
                        expected = &k_psx_game_dispatch[i];
                assert(psx_game_find_entry(addr) == expected);
                assert(psx_game_is_function_entry(addr) == (expected != 0));
                if (segments[s] != 0x00000000u) assert(!expected);
                int has_home = 0;
                for (unsigned i = 0; i < PSX_GAME_DISPATCH_COUNT; ++i)
                    if ((k_psx_game_dispatch[i].addr & 0x1FFFFFFFu) == phys) has_home = 1;
                if (has_home) assert(psx_game_address_in_text(addr));
                if (has_home && segments[s] != 0x00000000u) {
                    int old_calls = calls, old_irqs = irqs;
                    cpu.pc = 0xDEADBEEFu;
                    assert(!psx_game_text_native_ok(addr));
                    assert(!psx_game_text_native_ok_full(addr));
                    assert(!psx_dispatch_game_compiled(&cpu, addr));
                    assert(calls == old_calls && irqs == old_irqs && cpu.pc == 0xDEADBEEFu);
                }
                /* A PC in a 2nd-4th RAM mirror keeps its own address: the
                 * compiled body bakes the PCs it was compiled for into
                 * $ra/EPC, so it must never run for a mirror PC, and the
                 * mirror is not the EXE text. (Retail executes the folded
                 * bytes through the interpreter, with the mirror PC.) */
                for (uint32_t m = 1; m < 4; ++m) {
                    const uint32_t mirror = addr + m * 0x00200000u;
                    int old_calls = calls, old_irqs = irqs;
                    cpu.pc = 0xDEADBEEFu;
                    assert(psx_game_find_entry(mirror) == 0);
                    assert(!psx_game_address_in_text(mirror));
                    assert(!psx_game_text_native_ok(mirror));
                    assert(!psx_dispatch_game_compiled(&cpu, mirror));
                    assert(calls == old_calls && irqs == old_irqs &&
                           cpu.pc == 0xDEADBEEFu);
                }
                if (!expected) continue;
                home_hits++;
                // A previously resolved PC must NOT cache its live-byte verdict.
                allowed = 0; cpu.pc = 0xDEADBEEFu;
                int old_calls = calls, old_irqs = irqs;
                assert(!psx_game_text_native_ok(addr));
                assert(!psx_dispatch_game_compiled(&cpu, addr));
                assert(calls == old_calls && irqs == old_irqs && cpu.pc == 0xDEADBEEFu);
                allowed = 1;
                assert(psx_dispatch_game_compiled(&cpu, addr));
                assert(calls == old_calls + 1 && irqs == old_irqs + 1);
                assert(resumed == expected->resume_pc);
            }
        }
        assert(!psx_game_find_entry(0xFFFFFFFFu));
        assert(!psx_game_find_entry(0));
    }
    assert(home_hits >= 4);   /* both functions, both geometries */
    return 0;
}
"""
    compile_and_run(args.compiler, harness,
                    [os.path.join(runtime, "src", "psx_ram_geometry.c")], include)

    # The binary-search form of the same lookup (a table spanning 2 MiB or
    # more): every row answers only for its exact PC, in every segment; the
    # other segments' aliases and the words next to a row miss and never run.
    wide_fragment, wide_funcs = lookup_fragment(wide_src)
    wide = r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef struct { uint32_t pc; } CPUState;
static int calls = 0;
static void dummy(CPUState* cpu) { (void)cpu; calls++; }
static int dirty_ram_text_native_ok_ranges_from(const uint32_t* r, uint32_t n, uint32_t a) {
    (void)r; (void)n; (void)a; return 1;
}
static int dirty_ram_text_native_ok_ranges(const uint32_t* r, uint32_t n) {
    return dirty_ram_text_native_ok_ranges_from(r, n, 0);
}
static void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; }
int psx_vsync_query_hle_try(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
"""
    wide += "\n".join(f"#define {fn} dummy" for fn in wide_funcs) + "\n" + wide_fragment
    wide += r"""
int main(void) {
    const uint32_t segments[] = {0x00000000u, 0x80000000u, 0xA0000000u};
    CPUState cpu = {0};
    unsigned hits = 0;
    assert(PSX_GAME_DISPATCH_COUNT >= 2);
    for (unsigned i = 0; i < PSX_GAME_DISPATCH_COUNT; ++i) {
        const uint32_t phys = k_psx_game_dispatch[i].addr & 0x1FFFFFFFu;
        for (unsigned s = 0; s < 3; ++s) {
            for (int d = -4; d <= 4; d += 4) {
                const uint32_t addr = segments[s] | (phys + (uint32_t)d);
                const PsxGameDispatchEntry* expected = 0;
                for (unsigned j = 0; j < PSX_GAME_DISPATCH_COUNT; ++j)
                    if (k_psx_game_dispatch[j].addr == addr) expected = &k_psx_game_dispatch[j];
                assert(psx_game_find_entry(addr) == expected);
                assert(psx_game_is_function_entry(addr) == (expected != 0));
                if (segments[s] != 0x00000000u) assert(!expected);
                const int before = calls;
                assert(psx_dispatch_game_compiled(&cpu, addr) == (expected != 0));
                assert(calls == before + (expected != 0));
                hits += expected != 0;
            }
        }
    }
    assert(hits == PSX_GAME_DISPATCH_COUNT);
    return 0;
}
"""
    compile_and_run(args.compiler, wide, [], include)

    print("KUSEG-linked EXE test passed (%d KUSEG rows, exact lookup in the indexed and "
          "binary-search forms, aliases miss, variant seeds reported, KSEG1 image charged "
          "per instruction, foreign config sites and directives refused, physically "
          "matched sites accepted, mirror PCs never run compiled code in 2 or 8 MiB "
          "geometry)" % len(rows))


if __name__ == "__main__":
    main()
