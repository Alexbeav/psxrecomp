/* PS1B-248: MFC0/CFC0 read COP0 as it stood when the instruction began.
 *
 * Inputs: the reference RAM image at Bio Hazard return 191,809 and PSX-SPX's
 * COP0 CAUSE description only. The kernel's `mfc0 a1,Cause` at 0xD4C charges one
 * cycle, and a CD IRQ falls due on exactly that cycle. The reference stores CAUSE
 * without IP2 (0x20); the IRQ is visible from the next instruction on. Here the
 * device service stub raises IP2 when the instruction's own charge reaches the
 * deadline, which is the order the runtime's inlined psx_advance_cycles uses. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
int l1_step(CPUState *, uint32_t, uint32_t, uint32_t *);

/* Clock and device seams read as data on the charged path. */
uint64_t psx_cycle_count, psx_next_service_cycle, g_psx_cycle_fast_limit;
int psx_in_device_service, g_event_step_conservative, g_psx_cyc_bb_defer;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit, *g_psx_cyc_local_acc;
int g_input_instruction_histogram_active;
static CPUState *device_cpu;
static unsigned services;
void psx_devices_service_to_now(void) {
    ++services;
    device_cpu->cop0[13] |= 0x400u;     /* the INTC line: CAUSE.IP2 */
    psx_next_service_cycle = UINT64_MAX;
}
void psx_cpu_step_boundary_fn(CPUState *c, uint32_t a) { (void)c; (void)a; }
void psx_icache_fetch_miss(CPUState *c, uint32_t a) { (void)c; (void)a; }
int source_gpu_runtime_active(void) { return 0; }
int psx_ws_backdrop_preload(void) { return 0; }
int psx_ws_is_cull_xclip_load_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_plane_nx_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_angle_site(uint32_t p,uint32_t i,uint32_t *v) {(void)p;(void)i;(void)v;return 0;}
int psx_ws_is_cull_bias_site(uint32_t p) {(void)p;return 0;}
int psx_ws_is_signed_x_bound_site(uint32_t p,uint32_t i) {(void)p;(void)i;return 0;}
void psx_pgxp_alu(CPUState *c,uint32_t i,uint32_t a,uint32_t b,uint32_t v) {(void)c;(void)i;(void)a;(void)b;(void)v;}
void psx_pgxp_move(CPUState *c,uint32_t i,uint32_t v) {(void)c;(void)i;(void)v;}

static unsigned checks;
static void equal(uint32_t got, uint32_t expected, const char *what) {
    ++checks;
    if (got != expected) { fprintf(stderr, "%s: %08x != %08x\n", what, got, expected); assert(got == expected); }
}
static void step(CPUState *c, uint32_t word) {
    uint32_t next; assert(l1_step(c, c->pc, word, &next) == 0); c->pc = next;
}
/* Run `word` (reading COP0 register 13 into a1) then two nops, so the delayed
 * value has committed. Returns what the instruction read. */
static uint32_t read_cause(uint32_t word, int due_on_own_cycle, int raised_before) {
    static CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x00000D4Cu; cpu.read_fudge = 32; cpu.ld_which_t = 32;
    cpu.cop0[13] = 0x20u | (raised_before ? 0x400u : 0u);    /* ExcCode 8 (syscall) */
    device_cpu = &cpu; services = 0;
    psx_cycle_count = 108586084250u;
    psx_next_service_cycle = due_on_own_cycle ? psx_cycle_count + 1u : UINT64_MAX;
    step(&cpu, word);
    equal(services, due_on_own_cycle ? 1u : 0u, "event ran inside the instruction's charge");
    step(&cpu, 0u); step(&cpu, 0u);   /* the slot, then the first reader */
    equal(cpu.cop0[13], 0x20u | ((due_on_own_cycle || raised_before) ? 0x400u : 0u), "CAUSE after the event");
    return cpu.gpr[5];
}

int main(void) {
    const uint32_t mfc0 = 0x40056800u, cfc0 = 0x40456800u;   /* mfc0/cfc0 a1,$13 */
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t w = i ? cfc0 : mfc0;
        /* The PS1B-248 case: the event falls due on the read's own cycle. */
        equal(read_cause(w, 1, 0), 0x20u, "read with the IRQ due on its own cycle");
        /* Controls: no event, and an IRQ already raised before the instruction. */
        equal(read_cause(w, 0, 0), 0x20u, "read with no event");
        equal(read_cause(w, 0, 1), 0x420u, "read with the IRQ raised before it");
    }
    printf("mfc0 event sample: %u checks passed\n", checks);
    return 0;
}
