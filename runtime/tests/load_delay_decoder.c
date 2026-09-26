/* Production decoder entry for authored L1 instruction-level fixtures. */
#include "../src/dirty_ram_interp.c"
int l1_step(CPUState *cpu, uint32_t pc, uint32_t word, uint32_t *next) {
    return exec_one_fetched(cpu, pc, word, next);
}
