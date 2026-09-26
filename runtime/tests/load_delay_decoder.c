/* Production decoder entry for authored L1 instruction-level fixtures. */
#define PSX_NO_DEBUG_TOOLS 1
#include "../src/dirty_ram_interp.c"
int l1_step(CPUState *cpu, uint32_t pc, uint32_t word, uint32_t *next) {
    return exec_one_fetched(cpu, pc, word, next);
}

void l1_precise_control(void) {g_precise_mode=1;}
void l1_inject_irq(CPUState *cpu,uint32_t pc) {
 cpu->cop0[13]|=0x400u;interp_exception(cpu,0,cpu->cop0[8],pc);
}

int l1_inject_syscall(CPUState *cpu) {return interp_exception(cpu,8,cpu->cop0[8],cpu->pc);}
