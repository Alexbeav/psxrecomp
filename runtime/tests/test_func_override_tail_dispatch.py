#!/usr/bin/env python3
"""Execute the production tail-entry block with the real override registry."""

import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--source", type=Path,
                        default=ROOT / "runtime/src/dirty_ram_interp.c")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    pump = source.index("dirty_ram_pump_boundary(cpu, target, 1)")
    start = source.index("uint32_t target_phys = target &", pump)
    end = source.index("\n            target = cpu->pc;", start)
    block = source[start:end]
    # Read the actual continuation predicate, including the old source for
    # the negative control. The surrounding device loop is not under test.
    local_start = source.index("if (", source.index("#endif", end) + 6)
    local_end = source.index("{", local_start)
    local_condition = source[local_start:local_end]
    harness = r'''
#define main override_registry_tests
#include "test_func_override.c"
#undef main
#define PSX_HAS_GAME_DISPATCH 1
#define OV_FPLOG_RET1() return 1
static int g_precise_mode, dirty, entry = 1, region = 1;
static int g_dirty_ram_blocks_run;
static uint32_t g_dirty_interp_chain_target;
static int overlay_loader_is_candidate(uint32_t addr) { (void)addr; return 0; }
static int psx_game_is_function_entry(uint32_t addr) { (void)addr; return entry; }
static int dirty_ram_is_dirty(uint32_t addr) { (void)addr; return dirty; }
static int phys_is_overlay_flow_region(uint32_t addr) { (void)addr; return region; }
static int tail(CPUState *cpu, uint32_t insn, uint32_t pc, uint32_t stop_addr,
                uint32_t current_function_entry_phys, int allow_local_dirty_flow)
{
    struct { uint64_t insns; } *pc_entry = NULL;
    int insns_executed = 2;
    uint32_t target = cpu->pc;
    /* PRODUCTION_BLOCK */
    /* LOCAL_CONDITION */ return 2;
    return 0;
}
static int prehook(CPUState *cpu) { cpu->gpr[4]++; return 0; }
static void run_case(int is_dirty, int local, int in_region, uint32_t stop)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.pc = 0x80018000u;
    cpu.gpr[31] = 0x80019000u;
    dirty = is_dirty; region = in_region;
    int route = tail(&cpu, 0x08006000u, 0x80017010u, stop, 0x17000u, local);
    if (route == 0 && cpu.pc != stop)
        func_override_try_dispatch(&cpu, cpu.pc, cpu.gpr[31]);
    CHECK(cpu.gpr[4] == (stop == 0x80018000u ? 0u : 1u),
          "one pre-hook per transfer: dirty=%d local=%d region=%d stop=%x count=%u",
          is_dirty, local, in_region, stop, cpu.gpr[4]);
}
int main(void)
{
    CHECK(func_override_add("tail.prehook", 0x80018000u, prehook, 0) == FO_OK,
          "register pre-hook");
    func_override_install();
    run_case(0, 1, 1, 0); /* Clean static target: only the outer dispatcher owns it. */
    run_case(1, 1, 1, 0); /* Dirty local target: only the interpreter owns it. */
    run_case(1, 0, 1, 0); /* Dirty target surfaced by this dispatch invocation. */
    run_case(1, 1, 0, 0); /* Target outside the local interpreter region. */
    run_case(1, 1, 1, 0x80018000u); /* Return boundary, not a fresh entry. */
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    dirty = region = 1;
    const uint32_t jumps[] = {0x08006000u, (8u << 21) | 8u, (31u << 21) | 8u};
    for (int i = 0; i < 3; ++i) {
        cpu.pc = 0x80018000u; cpu.gpr[4] = 0;
        tail(&cpu, jumps[i], 0x80017010u, 0, 0x17000u, 1);
        CHECK(cpu.gpr[4] == (i == 2 ? 0u : 1u), "J/JR/return entry policy");
    }
    for (int mode = 0; mode < 5; ++mode) {
        cpu.pc = 0x80018000u; cpu.gpr[4] = 0;
        g_precise_mode = mode == 0; g_ls_replay_active = mode == 1;
        entry = mode != 2;
        tail(&cpu, jumps[0], mode == 3 ? cpu.pc : 0x80017010u, 0,
             mode == 4 ? 0x18000u : 0x17000u, 1);
        CHECK(cpu.gpr[4] == 0, "precise/replay/non-entry/self/back-edge policy %d", mode);
    }
    g_precise_mode = g_ls_replay_active = 0; entry = 1;
    CHECK(func_override_add("tail.redirect", 0x80018100u, impl_redirects, 0) == FO_OK,
          "register handled redirect");
    cpu.pc = 0x80018100u;
    CHECK(tail(&cpu, jumps[0], 0x80017010u, 0, 0x17000u, 1) == 1,
          "handled local override exits the interpreter");
    CHECK(cpu.pc == 0x8000D00Du && g_dirty_interp_chain_target == cpu.pc,
          "handled override keeps its non-local continuation");
    if (g_fail) return 1;
    puts("tail override ownership: all checks passed");
    return 0;
}
'''.replace("/* PRODUCTION_BLOCK */", block).replace(
        "/* LOCAL_CONDITION */", local_condition)
    with tempfile.TemporaryDirectory(prefix="func-override-tail-") as temp:
        path = Path(temp)
        probe = path / "tail.c"
        exe = path / "tail.exe"
        probe.write_text(harness, encoding="utf-8")
        subprocess.run([args.cc, "-std=c99", "-O0", "-I", str(ROOT / "runtime/include"),
                        "-I", str(ROOT / "runtime/tests"), str(probe),
                        str(ROOT / "runtime/src/func_override.c"),
                        str(ROOT / "runtime/src/crc32.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
