/* ChangeThread to the running thread must continue after the SYSCALL.
 *
 * A game can yield from its main thread with ChangeThread(<main thread>)
 * while that thread is current; the kernel then reaches SYSCALL 3 (at 0x650
 * in the retail kernel). The dirty-RAM interpreter sets cpu->pc to the
 * SYSCALL before psx_syscall and transfers to cpu->pc when it is non-zero.
 * The HLE scheduler's same-thread branch returned without clearing it, so the
 * interpreter re-executed the SYSCALL forever. This drives the real traps.c
 * case 3 with the HLE scheduler (the default) and a one-thread TCB table. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"

#define PCB_ADDR  0x0000E000u /* [0x108]: the process control block */
#define TCB_BASE  0x0000E1F4u /* [0x110]: first TCB, the main thread */
#define TCB_SIZE  (4u * 0xC0u) /* [0x114]: four TCBs */
#define SYSCALL_PC 0x00000650u

static uint32_t ram[0x10000u / 4u];
static unsigned same_thread_events, other_thread_events;

static uint32_t read_word(uint32_t address) {
    assert(address < sizeof(ram) && (address & 3u) == 0u);
    return ram[address / 4u];
}
static void write_word(uint32_t address, uint32_t value) {
    (void)address; (void)value;
    assert(!"a same-thread ChangeThread must not store guest state");
}

/* Kernel state seams traps.c reads on this path. */
int psx_get_in_exception(void) { return 0; }
void debug_server_log_thread_event(uint32_t kind, CPUState *cpu,
                                   uint32_t current_tcb, uint32_t target_tcb,
                                   uint32_t target_pc) {
    (void)cpu; (void)target_pc;
    if (kind == 5u && current_tcb == TCB_BASE && target_tcb == TCB_BASE)
        ++same_thread_events;
    else if (kind != 3u && kind != 20u)
        ++other_thread_events;
}

int psx_syscall(CPUState *cpu, uint32_t code);

int main(void) {
    ram[0x108u / 4u] = PCB_ADDR;
    ram[0x110u / 4u] = TCB_BASE;
    ram[0x114u / 4u] = TCB_SIZE;
    ram[PCB_ADDR / 4u] = TCB_BASE;  /* the main thread is current */
    ram[TCB_BASE / 4u] = 0x4000u;   /* and runnable */

    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    for (unsigned i = 1; i < 32; ++i) cpu.gpr[i] = 0x55550000u + i;
    cpu.gpr[4] = 3u;         /* SYS(03h) */
    cpu.gpr[5] = TCB_BASE;   /* ChangeThread target: the current thread */
    cpu.gpr[31] = 0x00002104u;
    cpu.cop0[12] = 0x00000401u;
    cpu.cop0[13] = 0x00000400u;
    cpu.cop0[14] = 0x00000650u;
    cpu.read_word = read_word;
    cpu.write_word = write_word;

    /* The interpreter's SYSCALL case: publish the instruction, then run it. */
    cpu.pc = SYSCALL_PC;
    CPUState expected = cpu;
    expected.pc = 0u;        /* continue after the SYSCALL, as SYS01/02 do */

    int transfer = psx_syscall(&cpu, 0u);
    assert(transfer == 0);
    assert(same_thread_events == 1u && other_thread_events == 0u);
    /* cpu->pc != 0 is the interpreter's transfer test (dirty_ram_interp.c). */
    assert(cpu.pc == 0u);
    assert(memcmp(cpu.gpr, expected.gpr, sizeof(cpu.gpr)) == 0);
    assert(memcmp(cpu.cop0, expected.cop0, sizeof(cpu.cop0)) == 0);
    assert(cpu.hi == expected.hi && cpu.lo == expected.lo);
    assert(ram[PCB_ADDR / 4u] == TCB_BASE);
    return 0;
}
