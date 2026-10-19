/* Production dirty-RAM interpreter decoder, built with per-instruction cycle
 * charging on, for test_interp_cop0_read_sample.c. */
#define PSX_ENABLE_BLOCK_CYCLES 1
#define PSX_NO_DEBUG_TOOLS 1
#include "../src/dirty_ram_interp.c"

int interp_test_step(CPUState *cpu, uint32_t pc, uint32_t insn, uint32_t *next) {
    return exec_one_fetched(cpu, pc, insn, next);
}
