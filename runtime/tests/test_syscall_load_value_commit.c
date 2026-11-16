/* A pending deferred load is committed before psx_syscall runs (PS1B-529).
 *
 * The recompiled path defers a load's register write by one instruction to
 * model the PS1 load delay: psx_load_value_arm(cpu, rt, value) parks the write
 * and psx_load_value_commit(cpu) performs it. A syscall is an observation
 * boundary in the guest ABI, so the pending write must land BEFORE the syscall
 * reads a0 or writes v0. When it does not, two faults happen together:
 *
 *   1. the syscall number is read from the stale register, and
 *   2. the parked write lands after the syscall and overwrites the value the
 *      syscall returned, so the caller sees the loaded word instead of the
 *      BIOS result.
 *
 * Symptom in the field: WipEout (Europe) takes a gcc divide-by-zero at
 * 0x80077228 with a zero divisor, and Nightmare Creatures (USA) parks in a
 * BIOS wait with pc 0. Both clear with PSX_SYSCALL_MODEL=guest-exception,
 * which returns before this branch is reached, so the branch itself is the
 * suspect. Pin H (a279b5e8) opened psx_syscall with this commit; 0.1.9
 * (5a716b69) does not.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FX_CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); fflush(stdout); _Exit(1); } } while (0)

#include "cpu_state.h"

/* Runtime data the syscall module refers to; the fixture supplies it. */
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;

static uint32_t fx_read0(uint32_t addr) { (void)addr; return 0u; }
static void fx_write0(uint32_t addr, uint32_t value) { (void)addr; (void)value; }

int main(void) {
    CPUState cpu;

    /* --- EnterCriticalSection with a load parked on v0 ------------------ */
    memset(&cpu, 0, sizeof(cpu));
    cpu.read_word = fx_read0;
    cpu.write_word = fx_write0;
    cpu.pc = 0x80001000u;
    cpu.gpr[4] = 1u;              /* a0 = SYS(01h) EnterCriticalSection */
    cpu.cop0[12] = 0x00000401u;   /* SR: IEc set, IM[2] set           */

    /* The instruction before the syscall loaded v0 and the write is parked. */
    psx_load_value_arm(&cpu, 2u, 0xDEADBEEFu);
    FX_CHECK(cpu.load_value_rt == 2u);

    (void)psx_syscall(&cpu, 0u);

    /* The parked write must be gone: the syscall owns v0 from here. */
    FX_CHECK(cpu.load_value_rt == 0u);
    /* And the value the caller sees is the syscall's own return, not the
     * loaded word parked by the instruction before it. */
    FX_CHECK(cpu.gpr[2] == 1u);

    /* --- ExitCriticalSection, same contract --------------------------- */
    memset(&cpu, 0, sizeof(cpu));
    cpu.read_word = fx_read0;
    cpu.write_word = fx_write0;
    cpu.pc = 0x80002000u;
    cpu.gpr[4] = 2u;              /* a0 = SYS(02h) ExitCriticalSection */
    cpu.cop0[12] = 0x00000400u;   /* SR: IEc clear                     */
    cpu.gpr[2] = 0x1234u;         /* a value from an earlier BIOS call  */

    psx_load_value_arm(&cpu, 2u, 0xBAD0BAD0u);
    (void)psx_syscall(&cpu, 0u);

    FX_CHECK(cpu.load_value_rt == 0u);
    /* The parked load lands at the syscall boundary, so v0 already holds the
     * loaded word; SYS(02h) then preserves it (it has no return value of its
     * own). Without the commit, the write would land after this point and
     * overwrite whatever a later instruction put in v0. */
    FX_CHECK(cpu.gpr[2] == 0xBAD0BAD0u);

    printf("PASS: psx_syscall commits a parked load before it reads or writes registers\n");
    return 0;
}
