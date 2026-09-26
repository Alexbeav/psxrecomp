/* render_pass.c - host-timed render passes (psx_mod_render_pass_plan /
 * psx_mod_render_pass, mod_plugins.h). See docs/RENDER_PASSES.md.
 *
 * A pass is a sandboxed nested guest call. The guest-visible machine is
 * checkpointed, time is frozen (psx_cycle_freeze.h), the plugin draws an image of
 * the scene through the game's own code, the OpenGL backend captures the
 * declared display rect, and then everything is put back: CPU (with the
 * GTE), RAM, scratchpad, I-cache tags, I_STAT/I_MASK, timers, DMA and GPU
 * registers, and the VRAM rect. Stores that could escape that restore (SPU,
 * CD, timers, other DMA channels, ...) never reach their device in the first
 * place (memory.c render_pass_store).
 *
 * Nothing here runs unless a trusted plugin calls the API, and the plan
 * refuses in netplay, rollback, rewind, turbo, self-check resimulation, or
 * without the OpenGL presenter's flip-aware interpolation. */

#include "render_pass.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "dma.h"
#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "mod_plugins.h"
#include "psx_icache.h"
#include "timers.h"

extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern uint32_t i_stat;
extern uint32_t i_mask;
extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t *p);
extern int      dma_snapshot_read(const uint8_t *p, uint32_t len);
extern uint32_t interrupts_get_cycles_since_vblank(void);
extern void     interrupts_set_cycles_since_vblank(uint32_t v);
extern void     psx_irq_refresh_cause_ip2(void);
extern int      psx_get_in_exception(void);
extern int      psx_netplay_active(void);
extern int      psx_netplay_is_resimulating(void);
extern int      psx_selfcheck_resim_active(void);
extern int      psx_rewind_is_open(void);
extern uint64_t g_guest_store_count;
extern uint64_t g_mmio_access_count;
extern uint32_t g_debug_last_store_pc;
extern uint32_t g_debug_current_func_addr;
extern int      g_psx_dispatch_depth;
extern int      g_psx_call_bail;
extern int      g_ls_mode;
extern int      g_ls_replay_active;

#define RP_RAM_SIZE   (2u * 1024u * 1024u)
#define RP_SPAD_SIZE  1024u
/* ~8 M guest cycles is about four PS1 frames of CPU time: far beyond any
 * frame's draw code, short enough that a pass stuck on a wait loop costs the
 * host well under a frame before it is rolled back. */
#define RP_WATCHDOG_CYCLES 8000000u
/* Faults (watchdog, VRAM leaks) before passes stay off for the session. */
#define RP_FAULT_LIMIT 8u

typedef struct RenderPassCheckpoint {
    CPUState cpu;
    uint8_t  spad[RP_SPAD_SIZE];
    uint32_t icache[1024];
    uint32_t i_stat, i_mask, csv;
    uint16_t t_counter[3], t_target[3];
    uint32_t t_mode[3], t_frac[3];
    int32_t  t_irq[3];
    uint64_t store_count, mmio_count;
    uint32_t last_store_pc, current_func;
    int      dispatch_depth, call_bail;
    uint32_t dma_len;
} RenderPassCheckpoint;

static uint8_t *s_ram_copy;          /* RP_RAM_SIZE */
static uint8_t *s_dma_copy;
static RenderPassCheckpoint s_ck;
static PsxCycleFreeze s_freeze;
static jmp_buf s_abort_jmp;
static volatile int s_abort_armed;
static int s_nesting;

static RenderPassStats s_stats;
static int s_verify = -1;
static int s_open_generation;        /* next pass captures frame N's image */
static uint32_t s_plan_period = 2;

static int verify_on(void) {
    if (s_verify < 0) {
        const char *e = getenv("PSX_RENDER_PASS_VERIFY");
        s_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_verify;
}

void render_pass_get_stats(RenderPassStats *out) {
    if (!out) return;
    *out = s_stats;
    for (int i = 0; i < RENDER_PASS_DROP_CLASSES; i++)
        out->dropped[i] = g_render_pass_dropped_writes[i];
}

void render_pass_reset_session(void) {
    memset(&s_stats, 0, sizeof s_stats);
    memset(g_render_pass_dropped_writes, 0, sizeof g_render_pass_dropped_writes);
    s_open_generation = 0;
}

/* Everything that must hold before guest code may run frozen. */
static int passes_allowed(void) {
    if (s_stats.disabled || s_nesting || g_psx_render_pass_active) return 0;
    if (psx_netplay_active() || psx_netplay_is_resimulating()) return 0;
    if (psx_selfcheck_resim_active() || psx_rewind_is_open()) return 0;
    if (g_ls_mode || g_ls_replay_active) return 0;
    if (psx_get_in_exception()) return 0;
    if (dma_gpu_linked_list_active()) return 0;
    return 1;
}

uint32_t psx_mod_render_pass_plan(uint32_t period_vblanks,
                                  uint32_t shown_after_vblanks,
                                  uint32_t *alpha_q16, uint32_t max) {
    uint32_t wanted = 0, n;
    s_open_generation = 0;
    if (!alpha_q16 || max == 0 || period_vblanks == 0 || period_vblanks > 8 ||
        shown_after_vblanks > 8)
        return 0;
    if (!passes_allowed()) return 0;
    n = gl_renderer_pass_plan(period_vblanks, shown_after_vblanks,
                              alpha_q16, max, &wanted);
    s_stats.wanted += wanted;
    if (!n) {
        if (wanted) s_stats.refused++;
        return 0;
    }
    s_stats.plans++;
    s_stats.planned += n;
    s_open_generation = 1;
    s_plan_period = period_vblanks;
    return n;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}

/* Guest-visible state hash for PSX_RENDER_PASS_VERIFY. */
static uint64_t state_hash(const CPUState *cpu) {
    uint64_t h = 1469598103934665603ULL;
    uint16_t tc[3], tt[3];
    uint32_t tm[3], tf[3];
    int32_t ti[3];
    uint32_t csv = interrupts_get_cycles_since_vblank();
    uint64_t cyc = psx_cycle_count;
    h = fnv(h, cpu->gpr, sizeof cpu->gpr);
    h = fnv(h, &cpu->hi, sizeof cpu->hi);
    h = fnv(h, &cpu->lo, sizeof cpu->lo);
    h = fnv(h, cpu->cop0, sizeof cpu->cop0);
    h = fnv(h, cpu->gte_data, sizeof cpu->gte_data);
    h = fnv(h, cpu->gte_ctrl, sizeof cpu->gte_ctrl);
    h = fnv(h, memory_get_ram_ptr(), RP_RAM_SIZE);
    h = fnv(h, memory_get_scratchpad_ptr(), RP_SPAD_SIZE);
    h = fnv(h, g_psx_icache_tv, sizeof g_psx_icache_tv);
    h = fnv(h, &i_stat, sizeof i_stat);
    h = fnv(h, &i_mask, sizeof i_mask);
    h = fnv(h, &csv, sizeof csv);
    h = fnv(h, &cyc, sizeof cyc);
    timers_get_snapshot(tc, tm, tt, ti, tf);
    h = fnv(h, tc, sizeof tc); h = fnv(h, tm, sizeof tm);
    h = fnv(h, tt, sizeof tt); h = fnv(h, ti, sizeof ti); h = fnv(h, tf, sizeof tf);
    if (s_dma_copy) {
        uint32_t n = dma_snapshot_bytes();
        uint8_t *tmp = (uint8_t *)malloc(n);
        if (tmp) { dma_snapshot_write(tmp); h = fnv(h, tmp, n); free(tmp); }
    }
    {
        uint64_t g = gpu_pass_state_hash();
        h = fnv(h, &g, sizeof g);
    }
    return h;
}

static int checkpoint_save(const CPUState *cpu) {
    uint32_t dma_len = dma_snapshot_bytes();
    if (!s_ram_copy) {
        s_ram_copy = (uint8_t *)malloc(RP_RAM_SIZE);
        if (!s_ram_copy) return 0;
    }
    if (!s_dma_copy) {
        s_dma_copy = (uint8_t *)malloc(dma_len);
        if (!s_dma_copy) return 0;
    }
    if (!gpu_pass_checkpoint_save()) return 0;
    s_ck.cpu = *cpu;
    memcpy(s_ram_copy, memory_get_ram_ptr(), RP_RAM_SIZE);
    memcpy(s_ck.spad, memory_get_scratchpad_ptr(), RP_SPAD_SIZE);
    memcpy(s_ck.icache, g_psx_icache_tv, sizeof s_ck.icache);
    s_ck.i_stat = i_stat;
    s_ck.i_mask = i_mask;
    s_ck.csv = interrupts_get_cycles_since_vblank();
    timers_get_snapshot(s_ck.t_counter, s_ck.t_mode, s_ck.t_target,
                        s_ck.t_irq, s_ck.t_frac);
    s_ck.dma_len = dma_len;
    dma_snapshot_write(s_dma_copy);
    s_ck.store_count = g_guest_store_count;
    s_ck.mmio_count = g_mmio_access_count;
    s_ck.last_store_pc = g_debug_last_store_pc;
    s_ck.current_func = g_debug_current_func_addr;
    s_ck.dispatch_depth = g_psx_dispatch_depth;
    s_ck.call_bail = g_psx_call_bail;
    return 1;
}

static void checkpoint_restore(CPUState *cpu) {
    gpu_pass_checkpoint_restore();
    (void)dma_snapshot_read(s_dma_copy, s_ck.dma_len);
    timers_set_snapshot(s_ck.t_counter, s_ck.t_mode, s_ck.t_target,
                        s_ck.t_irq, s_ck.t_frac);
    interrupts_set_cycles_since_vblank(s_ck.csv);
    i_stat = s_ck.i_stat;
    i_mask = s_ck.i_mask;
    memcpy(memory_get_ram_ptr(), s_ram_copy, RP_RAM_SIZE);
    memcpy(memory_get_scratchpad_ptr(), s_ck.spad, RP_SPAD_SIZE);
    memcpy(g_psx_icache_tv, s_ck.icache, sizeof s_ck.icache);
    *cpu = s_ck.cpu;
    psx_irq_refresh_cause_ip2();
    g_guest_store_count = s_ck.store_count;
    g_mmio_access_count = s_ck.mmio_count;
    g_debug_last_store_pc = s_ck.last_store_pc;
    g_debug_current_func_addr = s_ck.current_func;
    g_psx_dispatch_depth = s_ck.dispatch_depth;
    g_psx_call_bail = s_ck.call_bail;
}

static double s_ms_per_tick = 0.0;
static double ema_ms(double cur, uint64_t ticks) {
    double ms = (double)ticks * s_ms_per_tick;
    return cur > 0.0 ? cur * 0.9 + ms * 0.1 : ms;
}

static void watchdog_overrun(void) {
    s_stats.watchdog++;
    s_stats.watchdog_flag = 1;
    if (s_abort_armed) {
        s_abort_armed = 0;
        longjmp(s_abort_jmp, 1);
    }
}

static void note_fault(const char *what) {
    s_stats.aborted++;
    if (s_stats.aborted <= 4)
        fprintf(stderr, "psxrecomp: render pass rolled back (%s)\n", what);
    if (s_stats.watchdog + s_stats.vram_leaks >= RP_FAULT_LIMIT &&
        !s_stats.disabled) {
        s_stats.disabled = 1;
        fprintf(stderr, "psxrecomp: render passes disabled for this session "
                "after %u faults\n", (unsigned)RP_FAULT_LIMIT);
    }
}

int psx_mod_render_pass(struct CPUState *cpu, const PSXModRenderPass *pass,
                        PSXModRenderPassFn fn, void *user) {
    uint64_t t0, t1, tb, tg, te, tr, hash_before = 0, hash_after = 0;
    uint64_t cycles_before;
    uint32_t leaks;
    int ok = 0, open;
    static uint32_t s_leaks_before;

    if (!cpu || !pass || !fn || pass->struct_size < sizeof *pass ||
        pass->w == 0 || pass->h == 0 || pass->alpha_q16 == 0 ||
        pass->alpha_q16 >= 65536u)
        return 0;
    if (!passes_allowed() || !gl_renderer_pass_ready()) return 0;

    t0 = gl_renderer_perf_ticks();
    open = s_open_generation;
    {
        static double inv = 0.0;
        if (inv == 0.0) inv = 1000.0 / (double)gl_renderer_perf_frequency();
        s_ms_per_tick = inv;
    }
    /* Frame N's own image is captured by the first pass after a plan. */
    if (!gl_renderer_pass_begin(pass->x, pass->y, pass->w, pass->h, open,
                                s_plan_period))
        return 0;
    s_open_generation = 0;
    s_leaks_before = gl_renderer_pass_leaks();

    if (!checkpoint_save(cpu)) {
        gl_renderer_pass_end(0, 0);
        return 0;
    }
    if (verify_on()) hash_before = state_hash(cpu);

    tb = gl_renderer_perf_ticks();
    s_nesting = 1;
    cycles_before = psx_cycle_count;
    (void)psx_cycle_freeze_begin(&s_freeze, RP_WATCHDOG_CYCLES,
                                 watchdog_overrun);
    if (setjmp(s_abort_jmp) == 0) {
        s_abort_armed = 1;
        ok = fn(cpu, user, pass->alpha_q16) ? 1 : 0;
        s_abort_armed = 0;
        if (!ok) s_stats.discarded++;
    } else {
        ok = 0;
    }
    s_stats.guest_cycles_last = psx_cycle_count - cycles_before;
    tg = gl_renderer_perf_ticks();

    leaks = gl_renderer_pass_leaks() - s_leaks_before;
    if (leaks) {
        s_stats.vram_leaks += leaks;
        ok = 0;
    }
    /* Capture (when kept), then roll the VRAM rect and renderer back. */
    gl_renderer_pass_end(ok ? pass->alpha_q16 : 0, ok);
    te = gl_renderer_perf_ticks();
    checkpoint_restore(cpu);
    psx_cycle_freeze_end(&s_freeze);
    s_nesting = 0;
    tr = gl_renderer_perf_ticks();
    s_stats.avg_begin_ms = ema_ms(s_stats.avg_begin_ms, tb - t0);
    s_stats.avg_guest_ms = ema_ms(s_stats.avg_guest_ms, tg - tb);
    s_stats.avg_end_ms = ema_ms(s_stats.avg_end_ms, te - tg);
    s_stats.avg_restore_ms = ema_ms(s_stats.avg_restore_ms, tr - te);

    if (verify_on()) {
        hash_after = state_hash(cpu);
        s_stats.verify_checks++;
        if (hash_after != hash_before || !gl_renderer_pass_verify_vram()) {
            s_stats.verify_mismatch++;
            if (s_stats.verify_mismatch <= 8)
                fprintf(stderr, "psxrecomp: RENDER PASS VERIFY mismatch "
                        "(state %016llx -> %016llx)\n",
                        (unsigned long long)hash_before,
                        (unsigned long long)hash_after);
        }
    }

    t1 = gl_renderer_perf_ticks();
    {
        double ms = (double)(t1 - t0) * 1000.0 /
                    (double)gl_renderer_perf_frequency();
        s_stats.last_pass_ms = ms;
        s_stats.avg_pass_ms = s_stats.avg_pass_ms > 0.0
            ? s_stats.avg_pass_ms * 0.9 + ms * 0.1 : ms;
    }
    gl_renderer_pass_note_cost(t1 - t0);
    if (ok) s_stats.passes++;
    else if (leaks) note_fault("VRAM write outside the pass rect");
    else if (s_stats.watchdog_flag) note_fault("guest-cycle watchdog");
    s_stats.watchdog_flag = 0;
    /* Present anything that fell due while the pass ran. */
    gl_renderer_pass_service_presents();
    return ok;
}
