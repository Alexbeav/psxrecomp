/* Collaborators for direct exec_one_fetched opcode fixtures. No source-device
 * profile, scheduled IRQ, AOT dispatcher or timed block is active here.
 * I-cache storage and decoding remain owned by the real psx_icache.c. */
#include "psx_icache.h"
#include "psx_cycles.h"
#include "interrupts.h"
#include <stdlib.h>

uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
int g_event_step_conservative, psx_in_device_service;
int g_interp_boundary_in_progress;
uint64_t g_irq_deliver_count;
uint64_t psx_next_service_cycle = UINT64_MAX;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
int g_psx_gcs_batch;

uint64_t psx_get_cycle_count(void) {
    return psx_cycle_count + g_psx_cyc_batch +
           (g_psx_cyc_local_acc ? *g_psx_cyc_local_acc : 0);
}
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }
int source_gpu_runtime_active(void) { return 0; }
int psx_interrupt_cooldown_active(void) { return 0; }
int psx_is_dispatchable(uint32_t pc) { (void)pc; return 0; }
/* Unexpected device delivery or BREAK must fail, not simulate success. */
void psx_devices_service_to_now(void) { abort(); }
int psx_irq_opcode_eligible(uint32_t pc) { (void)pc; abort(); }
int psx_break_enter_guest_vector(CPUState *cpu, uint32_t pc) {
    (void)cpu; (void)pc; abort();
}
int psx_check_interrupts_delay_slot(CPUState *cpu, uint32_t pc,
                                  uint32_t target, int taken, uint32_t word) {
    (void)cpu;(void)pc;(void)target;(void)taken;(void)word;abort();
}
