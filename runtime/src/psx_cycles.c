/* psx_cycles.c — PSX guest CPU cycle clock. */

#include "psx_cycles.h"
#include "cpu_state.h"
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>       /* MSVC intrinsics: _BitScanReverse (no __builtin_clz) */
#endif
#include "cdrom.h"
#include "dma.h"
#include "interrupts.h"
#include "sio.h"
#include "starvation_ring.h"
#include "timers.h"
#include "source_gpu_runtime.h"
#if defined(PSX_HAS_RECOMP_NET)
#include "psx_netplay.h"
#endif
#ifdef PSX_COSIM
#include "cosim_state.h"
#endif

uint64_t psx_cycle_count = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
int      g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = NULL;
static int      s_cycle_replay_active = 0;
static uint64_t s_cycle_replay_live = 0;

int psx_cycle_replay_begin(uint64_t start_cycle) {
    extern int g_ls_replay_active;
    if (!g_ls_replay_active || s_cycle_replay_active) return 0;
    s_cycle_replay_live = psx_cycle_count;
    psx_cycle_count = start_cycle;
    s_cycle_replay_active = 1;
    return 1;
}

uint64_t psx_cycle_replay_end(void) {
    uint64_t replay_cycle = psx_cycle_count;
    if (s_cycle_replay_active) {
        psx_cycle_count = s_cycle_replay_live;
        s_cycle_replay_active = 0;
    }
    return replay_cycle;
}

/* Throttle watchdog check to once per ~64K cycles (header hot path). */
uint32_t psx_watchdog_throttle = 0;
uint32_t psx_pc_sample_throttle = 0;

/* Conservative event-granularity diagnostic (set via debug cmd
 * overlay_native_event_granularity). Normally psx_advance_cycles charges a
 * whole basic block's cycles in ONE step, so every device advances N cycles at
 * once and any events that came due at sub-block cycles all fire together, in
 * the fixed device order below (sio,cdrom,dma,timers,interrupts) — NOT in true
 * due-cycle order. The dirty-RAM interpreter avoids this only because it calls
 * us with N=1 per instruction. When this flag is set, a batched (N>1) advance
 * is split into N single-cycle steps, so device events fire at their true
 * due-cycle in order — i.e. native execution gets the same event timeline the
 * interpreter produces. Diagnostic: if the village->overworld blue screen
 * clears with this on, the root cause is per-block event-ordering, and the
 * real fix is a due-cycle event scheduler (run-to-next-event), not this. */
int g_event_step_conservative = 0;

/* Env opt-in (PSX_EVENT_STEP_CONSERVATIVE=1) so the diagnostic is reachable in
 * production builds too, where the TCP setter doesn't exist — the determinism
 * A/B ("do the fast-limit and conservative paths present the same guest event
 * timeline?") needs the production binary to run BOTH ways. */
void psx_event_step_conservative_env_init(void) {
    const char *e = getenv("PSX_EVENT_STEP_CONSERVATIVE");
    if (e && e[0] == '1') g_event_step_conservative = 1;
}

static void advance_devices(uint32_t c) {
    { extern uint64_t g_psx_device_gen; g_psx_device_gen++; }   /* device state may change below */
    psx_cycle_count += (uint64_t)c;
    sio_advance(c);
    cdrom_advance(c);
    source_gpu_runtime_advance();
    dma_advance(c);
    timers_advance(c);
    interrupts_advance_cycles(c);
    psx_spu_sample_event_service();
}

/* ===== Event-deadline device servicing (production fast path) =================
 *
 * The faithful core charges guest cycles PER INSTRUCTION (psx_cyc.h §1), and the
 * legacy path below walked EVERY device on EVERY charge — ~10 host calls per
 * guest instruction, ~300M calls/s: the whole prod build pegged a core inside
 * psx_advance_cycles (gdb-sampled 14/14, leaves = timers/cdrom/dma/interrupts).
 *
 * FAITHFUL_TIMING_PLAN.md's target architecture is the fix: cycles accumulate
 * into the ONE global counter cheaply; devices are only SERVICED when the next
 * event deadline is reached (cycles_to_next_event(): min over vblank / timer /
 * cdrom / dma / sio IRQ distances), or when guest code touches device MMIO
 * (memory.c calls psx_devices_mmio_sync() so reads see current state and writes
 * that re-arm a device force a deadline recompute).
 *
 * Guest-visible semantics are IDENTICAL: servicing rewinds psx_cycle_count to
 * the devices' synced position and re-plays the gap through the SAME
 * vblank-bounded chunk loop the legacy path used, so device code observes the
 * exact same cycle values it always did, and no event can fire late because the
 * deadline is by construction <= the earliest device event (hard-capped at
 * DEADLINE_HARD_CAP for IRQ-masked device progress + frame pacing).
 *
 * The exact per-charge path is kept verbatim for PSX_COSIM builds (oracle
 * determinism / checkpoint alignment) and for the g_event_step_conservative
 * diagnostic toggle. */
#define PSX_DEADLINE_HARD_CAP 16384u

static uint64_t s_devices_synced_cycle = 0;  /* devices are advanced up to here */
/* Exported for the inlined psx_advance_cycles / psx_cyc_charge hot path. */
uint64_t psx_next_service_cycle = 0;         /* absolute; 0 = dirty, recompute  */
int      psx_in_device_service  = 0;         /* re-entrancy guard               */
int      g_plp_cycle_diag       = 0;
uint64_t g_plp_adv_calls        = 0;
uint32_t g_plp_adv_max_chunk    = 0;
uint64_t g_plp_adv_sum          = 0;
uint64_t g_plp_svc_calls        = 0;
#define s_next_service_cycle psx_next_service_cycle
#define s_in_device_service  psx_in_device_service
static uint64_t s_next_watchdog        = 0;
static uint64_t s_next_pc_sample       = 0;

/* Absolute inclusive limit for dirty_ram_interp.c's exact one-cycle path.
 * Zero means that the next charge must visit psx_advance_cycles(). */
uint64_t g_psx_cycle_fast_limit = 0;

/* Distance to the nearest INTERNAL device event, mask-blind (i_mask =
 * all-unmasked). This is the chunking bound for catch-up: the sio/cdrom/dma
 * *_advance() functions fire at most ONE event boundary per call (their
 * legacy caller stepped 1 cycle at a time), so every catch-up chunk must land
 * exactly ON the nearest event so chained sequences (SIO byte -> ack -> next
 * byte; CD sector trains) replay event-by-event, and so events armed while
 * their IRQ is masked in I_MASK still process on time (games poll I_STAT). */
static uint32_t devices_cycles_to_next_internal_event(void) {
    uint32_t best = interrupts_cycles_to_vblank();   /* frame pacing always */
    uint32_t t = timers_cycles_to_irq(0xFFFFFFFFu);  if (t < best) best = t;
    uint32_t c = cdrom_cycles_to_irq(0xFFFFFFFFu);   if (c < best) best = c;
    uint32_t d = dma_cycles_to_internal_event();     if (d < best) best = d;
    uint32_t g = source_gpu_runtime_cycles_to_event(); if (g < best) best = g;
    uint32_t s = sio_cycles_to_irq(0xFFFFFFFFu);     if (s < best) best = s;
    uint32_t a = psx_spu_sample_event_cycles_to_next(); if (a < best) best = a;
    if (best == 0) best = 1;    /* due/overdue: process within one cycle */
    return best;
}

/* Wait-loop observation boundary. Device catch-up remains mask-blind and
 * replays every intermediate timer/DMA/SIO transition, but a CPU loop with no
 * stores or MMIO cannot observe a masked IRQ until it becomes deliverable.
 * Stopping at every masked timer target turned a safe VBlank/CD wait skip into
 * tens of thousands of host-side re-detections per second. VBlank remains an
 * unconditional ceiling so frame pacing is never crossed in one jump. */
static uint32_t devices_cycles_to_next_idle_event(void) {
    extern uint32_t i_mask;
    uint32_t best = interrupts_cycles_to_vblank();
    uint32_t t = timers_cycles_to_irq(i_mask);  if (t < best) best = t;
    uint32_t c = cdrom_cycles_to_irq(i_mask);   if (c < best) best = c;
    uint32_t d = dma_cycles_to_deliverable_irq(i_mask); if (d < best) best = d;
    uint32_t s = sio_cycles_to_irq(i_mask);     if (s < best) best = s;
    /* SPU IRQ9 is raised by the guest-clock sample scheduler, not by a device
     * *_advance(); a poll loop waiting on it must not be skipped across several
     * 768-cycle sample boundaries before it can acknowledge and re-arm. Only an
     * unmasked IRQ9 is observable, like the other sources above. */
    if (i_mask & (1u << IRQ_SPU)) {
        uint32_t a = psx_spu_sample_event_cycles_to_next(); if (a < best) best = a;
    }
    if (best == 0) best = 1;
    return best;
}

/* Test/diagnostic accessor for the idle-skip observation boundary above. */
uint32_t psx_idle_cycles_to_next_observable_event(void) {
    return devices_cycles_to_next_idle_event();
}

/* Device-state generation (step 4b of the TAS speed task). Bumped whenever
 * device state can change: any device advance, scheduled-event service, MMIO
 * access (reads with side effects at the top, writes on wrapper exit) and the
 * explicit deadline-dirty sites. While it is unchanged, every device's next
 * internal event is at the same absolute clock, so the seven countdown queries
 * need not be repeated on every block leader. Enabled by PSX_DEADLINE_CACHE=1
 * (run_native.py --deadline-cache on); default recomputes exactly as before. */
uint64_t g_psx_device_gen;
int g_psx_deadline_cache = -1;
static uint64_t s_deadline_cache_gen = ~0ull, s_deadline_cache_abs;
uint64_t g_psx_deadline_cache_hits, g_psx_deadline_cache_misses;
static void psx_devices_recompute_deadline(void) {
    if (g_psx_deadline_cache < 0) { const char *e = getenv("PSX_DEADLINE_CACHE"); g_psx_deadline_cache = (e && e[0] == '1') ? 1 : 0; }
    uint32_t next;
    if (g_psx_deadline_cache && s_deadline_cache_gen == g_psx_device_gen) {
        g_psx_deadline_cache_hits++;
        next = s_deadline_cache_abs > psx_cycle_count ? (uint32_t)(s_deadline_cache_abs - psx_cycle_count) : 1u;
    } else {
        g_psx_deadline_cache_misses++;
        next = devices_cycles_to_next_internal_event();
        s_deadline_cache_gen = g_psx_device_gen;
        s_deadline_cache_abs = psx_cycle_count + (uint64_t)next;
    }
    if (next > PSX_DEADLINE_HARD_CAP) next = PSX_DEADLINE_HARD_CAP;
    psx_next_service_cycle = psx_cycle_count + (uint64_t)next;
}

void psx_devices_service_to_now(void) {
    if (s_in_device_service) return;                 /* device code charged cycles: absorb */
    if (g_plp_cycle_diag) g_plp_svc_calls++;
    g_psx_cycle_fast_limit = 0;
    s_in_device_service = 1;
    uint64_t target = psx_cycle_count;
    if (s_devices_synced_cycle < target) {
        /* Rewind and re-play the gap in event-bounded chunks so device
         * callbacks see the same incremental psx_cycle_count they always did
         * and no chunk ever skips OVER a device event boundary. */
        psx_cycle_count = s_devices_synced_cycle;
        interrupts_service_scheduled_events();
        while (psx_cycle_count < target) {
            uint32_t remaining = (uint32_t)(target - psx_cycle_count);
            uint32_t step = remaining;
            uint32_t to_ev = devices_cycles_to_next_internal_event();
            if (to_ev > 0 && to_ev < step) step = to_ev;
            if (step == 0) step = 1;
            /* Preserve interval causality at an advertised event boundary.
             * Devices are advanced in a fixed dependency order (CD before DMA,
             * MDEC input before output). Passing the whole D-cycle interval to
             * each device lets a state created by an earlier device at cycle D
             * be treated by a later device as though it existed for all D
             * cycles. Advance the quiet prefix first, then the boundary cycle:
             * this is exactly the conservative timeline with O(events), not
             * O(cycles), host calls. */
            if (step > 1u && to_ev == step) {
                advance_devices(step - 1u);
                step = 1u;
            }
            advance_devices(step);
            interrupts_service_scheduled_events();
        }
        s_devices_synced_cycle = target;
    }
    psx_devices_recompute_deadline();
    psx_in_device_service = 0;

    /* Sparse host maintenance (was on every psx_advance_cycles). HARD_CAP
     * guarantees we land here at least every 16K guest cycles. */
    if (target >= s_next_watchdog) {
        s_next_watchdog = target + 65536ull;
        psx_cycles_watchdog_fire();
#if defined(PSX_HAS_RECOMP_NET)
        /* INPUT/CONFIRM retransmit during BIOS free-run (before vblank admit). */
        if (psx_netplay_active())
            psx_netplay_pump();
#endif
    }
    if (target >= s_next_pc_sample) {
        s_next_pc_sample = target + 1048576ull;
        psx_cycles_pc_sample_fire();
    }
}

/* memory.c hook: called at the top of every device-MMIO read/write wrapper.
 * Catch devices up so the handler sees current state, then recompute the
 * deadline immediately (a write may re-arm a shorter event). Previously this
 * set psx_next_service_cycle=0 ("dirty"), forcing the *next* per-instruction
 * advance through service_to_now even when already synced — MotK FMV pays
 * that on every GPU/CD/MDEC MMIO touch. Recompute here instead. */
void psx_devices_mmio_sync(void) {
    g_psx_device_gen++;   /* MMIO reads can have side effects; writes bump again on wrapper exit */
    psx_cyc_batch_flush();
    if (s_devices_synced_cycle != psx_cycle_count) {
        psx_devices_service_to_now();
    } else {
        psx_devices_recompute_deadline();
    }
}

/* Exact per-charge path (legacy semantics). Used by PSX_COSIM builds and the
 * g_event_step_conservative diagnostic; also keeps the deadline-path state
 * coherent so the two can interleave (the runtime toggle flips mid-run). */
static void psx_advance_cycles_exact(uint32_t cycles) {
#ifdef PSX_COSIM
    /* First-divergence oracle: the guest-cycle counter is the ONLY clock both backends
     * share identically, so it is the alignment point for the full-state hash. */
    cosim_tick();
#endif
    interrupts_service_scheduled_events();
    while (cycles > 0) {
        uint32_t step = cycles;
        if (g_event_step_conservative && step > 1u) {
            step = 1u;
        } else {
            uint32_t to_vblank = interrupts_cycles_to_vblank();
            if (to_vblank > 0 && to_vblank < step) step = to_vblank;
#ifdef PSX_COSIM
            uint32_t to_cp = cosim_cycles_to_next_checkpoint();
            if (to_cp > 0 && to_cp < step) step = to_cp;
#endif
        }
        if (step == 0) step = cycles;
        advance_devices(step);
        cycles -= step;
        interrupts_service_scheduled_events();
#ifdef PSX_COSIM
        cosim_tick();
#endif
    }
    s_devices_synced_cycle = psx_cycle_count;
    psx_next_service_cycle = 0;
}

void psx_cycles_watchdog_fire(void) {
    starvation_watchdog_check();
}

void psx_cycles_pc_sample_fire(void) {
    starvation_ring_pc_sample();
}

/* Slow path for the inlined psx_advance_cycles (COSIM / lockstep / conservative). */
void psx_advance_cycles_slow(uint32_t cycles) {
    { extern int g_ls_replay_active;
      if (g_ls_replay_active) {
          /* Replay owns a private-in-time view of the global clock. Advance it
           * so GTE/muldiv deadlines and memory wait states see the same time as
           * the authoritative pass, but never service devices or observers. */
          if (s_cycle_replay_active) psx_cycle_count += (uint64_t)cycles;
          return;
      }
    }
    if (cycles == 0) return;
#ifdef PSX_COSIM
    psx_advance_cycles_exact(cycles);
#else
    if (g_event_step_conservative) {
        psx_advance_cycles_exact(cycles);
    } else if (psx_in_device_service) {
        psx_cycle_count += (uint64_t)cycles;
    } else {
        psx_cycle_count += (uint64_t)cycles;
        if (psx_next_service_cycle == 0 || psx_cycle_count >= psx_next_service_cycle) {
            psx_devices_service_to_now();
        }
    }
#endif
#if STARVATION_RING_ENABLED
    psx_watchdog_throttle += cycles;
    if (psx_watchdog_throttle >= 65536u) {
        psx_watchdog_throttle = 0;
        starvation_watchdog_check();
    }
    psx_pc_sample_throttle += cycles;
    if (psx_pc_sample_throttle >= 1048576u) {
        psx_pc_sample_throttle = 0;
        psx_cycles_pc_sample_fire();
    }
#endif
#if defined(PSX_HAS_RECOMP_NET)
    /* Independent of starvation ring (off in PSX_NO_DEBUG_TOOLS release).
     * Retransmit INPUT/CONFIRM during BIOS free-run before first vblank. */
    {
        static uint32_t s_net_pump_throttle;
        s_net_pump_throttle += cycles;
        if (s_net_pump_throttle >= 65536u) {
            s_net_pump_throttle = 0;
            if (psx_netplay_active())
                psx_netplay_pump();
        }
    }
#endif
}

uint64_t psx_get_cycle_count(void) {
    uint64_t n = psx_cycle_count + (uint64_t)g_psx_cyc_batch;
    if (g_psx_cyc_local_acc) n += (uint64_t)(*g_psx_cyc_local_acc);
    return n;
}


/* ===== Idle-loop cycle skip (wait-loop elision, 2026-07-06) ==================
 *
 * A pure poll loop (Tomba2 main loop: `do {} while (vbl_count < target)`)
 * burns its full guest-cycle budget at real emulation cost even though every
 * iteration is a provable no-op: no stores, no MMIO, and register state at
 * the loop's interrupt-check boundary identical each time. Nothing such a
 * loop can observe changes except via device servicing, which happens at
 * deterministic cycle deadlines. So time is fast-forwarded to the next
 * INTERNAL device event (mask-blind min over vblank pacing / timers / cdrom /
 * dma / sio — the full set of async guest-visible mutators) in whole loop
 * quanta, bit-exact vs executing the iterations:
 *   - real execution takes a deliverable IRQ at the first check boundary at
 *     or after the event cycle; the skip lands on exactly that boundary
 *     (k = ceil(dist / quantum) whole iterations).
 *   - device RAM writes (DMA) replay at their exact cycles inside
 *     psx_advance_cycles' deadline-servicing path either way.
 *   - if the event does not release the loop, the detector re-arms and
 *     skips to the next internal event.
 *
 * Detection is evidence-based; ALL gates must hold across IDLE_STREAK_MIN
 * consecutive interrupt checks at the SAME resume PC with a STABLE quantum:
 *   1. zero guest stores   (g_guest_store_count unchanged — covers RAM,
 *      scratchpad, MMIO and DMA writes; memory.c chokepoints)
 *   2. zero MMIO accesses  (g_mmio_access_count unchanged — an MMIO read can
 *      be side-effecting or time-varying, strictly disqualifying. I_STAT-
 *      polling kernel loops therefore never skip; whitelisting side-effect-
 *      free I_STAT reads is a possible later refinement.)
 *   3. identical GPR/hi/lo fingerprint at the check boundary — rules out
 *      progressing pure-load loops (strlen/memcmp-style scans) whose exit
 *      depends on register progress, not time. (COP0/GTE-held loop state is
 *      not fingerprinted; no realistic poll loop keys its exit on those.)
 *
 * Excluded contexts: exception delivery, bail unwinds, precise slicing,
 * lockstep record/replay, and PSX_COSIM builds entirely (the cosim oracle
 * compares per-instruction streams; elided iterations would false-diverge).
 *
 * Opt in: game config, PSX_IDLE_SKIP=1 (env), or the `idle_skip` TCP command.
 * The always-on counters below are the observability surface. */

enum {
    IDLE_STREAK_MIN      = 4,        /* stable same-PC windows before a skip */
    IDLE_QUANTUM_MAX     = 32768,    /* interp pump gap fits; junk deltas don't */
    IDLE_SKIP_MAX_CYCLES = 1200000   /* defensive cap (> one vblank interval)  */
};

int      g_idle_skip_enabled = -1;   /* -1 = read PSX_IDLE_SKIP once; TCP-togglable */
uint64_t g_idle_skip_count  = 0;     /* skips performed                */
uint64_t g_idle_skip_cycles = 0;     /* guest cycles fast-forwarded    */
uint32_t g_idle_skip_last_pc = 0;    /* loop PC of the last skip       */
uint32_t g_idle_skip_last_quantum = 0;
/* Overlay CPS code reports several interrupt-safe PCs per logical loop
 * iteration. The loader suppresses internal/return observations so they do not
 * erase evidence collected at the repeated external poll boundary; interrupt
 * delivery still runs normally. */
int g_idle_note_suppress = 0;

static uint32_t s_idle_pc = 0;
static uint32_t s_idle_quantum = 0;
static uint32_t s_idle_streak = 0;
static int      s_idle_have_snap = 0; /* GPR snapshot valid for s_idle_pc */
static uint32_t s_idle_gpr[32];
static uint32_t s_idle_hi = 0, s_idle_lo = 0;
static int      s_idle_progress_reg = -2; /* -2 unknown, -1 stable, 1..31 countdown */
static int32_t  s_idle_progress_delta = 0;
static uint64_t s_idle_last_cycle = 0;
static uint64_t s_idle_last_stores = 0;
static uint64_t s_idle_last_mmio = 0;

static void idle_snapshot_regs(const CPUState *cpu) {
    for (int i = 1; i < 32; i++) s_idle_gpr[i] = cpu->gpr[i];
    s_idle_hi = cpu->hi;
    s_idle_lo = cpu->lo;
}

static int idle_skip_on(void) {
    /* Cycle-skip is a host enhancement; under netplay it forks peers (detector
     * streak / skip quanta are not part of the shared snap). Same for the
     * solo resim self-check — keep the window on the faithful cycle path. */
    extern int psx_netplay_active(void);
    extern int psx_selfcheck_enabled(void);
    if (psx_netplay_active() || psx_selfcheck_enabled())
        return 0;
    if (g_idle_skip_enabled < 0) {
        /* No game config reached this process (for example a BIOS-only
         * runtime). Keep the enhancement inert unless the environment opts in;
         * normal game launches set g_idle_skip_enabled from RuntimeConfig. */
        const char *e = getenv("PSX_IDLE_SKIP");
        g_idle_skip_enabled = e ? (e[0] == '1') : 0;
    }
    return g_idle_skip_enabled;
}

int psx_idle_skip_is_enabled(void) { return idle_skip_on(); }

void psx_idle_note_check(CPUState *cpu, uint32_t check_pc) {
#ifdef PSX_COSIM
    (void)cpu; (void)check_pc;
    return;
#else
    extern int g_psx_call_bail, g_precise_mode, g_ls_mode, g_ls_replay_active;
    extern int psx_get_in_exception(void);
    extern uint64_t g_guest_store_count, g_mmio_access_count;

    /* A speculative replay must not clear or train the authoritative live idle
     * detector. Its clock/device view is transactional and cannot be skipped. */
    if (g_ls_replay_active) return;
    if (!idle_skip_on() || g_idle_note_suppress) return;
    if (check_pc == 0 || psx_get_in_exception() || g_psx_call_bail ||
        g_precise_mode || g_ls_mode != 0) {
        s_idle_pc = 0;
        s_idle_streak = 0;
        s_idle_have_snap = 0;
        return;
    }

    uint64_t cyc = psx_cycle_count;
    if (check_pc == s_idle_pc && s_idle_have_snap &&
        g_guest_store_count == s_idle_last_stores &&
        g_mmio_access_count == s_idle_last_mmio) {
        int changed = -1, changed_count = 0;
        for (int i = 1; i < 32; i++) {
            if (cpu->gpr[i] != s_idle_gpr[i]) {
                changed = i;
                changed_count++;
                if (changed_count > 1) break;
            }
        }
        if (cpu->hi != s_idle_hi || cpu->lo != s_idle_lo) changed_count = 2;
        int32_t progress_delta = changed_count == 1
            ? (int32_t)(cpu->gpr[changed] - s_idle_gpr[changed]) : 0;
        /* Besides the original invariant-register loop, accept exactly one
         * monotonically decrementing timeout register. Skipped iterations apply
         * the same decrements and stop before zero so the real exit branch still
         * executes. No stores/MMIO are allowed, so there is no hidden work. */
        int progress_reg = changed_count == 0 ? -1
                         : (changed_count == 1 && progress_delta == -1 ? changed : -2);
        uint32_t quantum = (uint32_t)(cyc - s_idle_last_cycle);
        if (progress_reg != -2 && quantum > 0 && quantum <= IDLE_QUANTUM_MAX) {
            if (quantum == s_idle_quantum &&
                progress_reg == s_idle_progress_reg &&
                progress_delta == s_idle_progress_delta) {
                if (s_idle_streak < 1000000u) s_idle_streak++;
            } else {
                s_idle_quantum = quantum;
                s_idle_progress_reg = progress_reg;
                s_idle_progress_delta = progress_delta;
                s_idle_streak = 1;
            }
        } else {
            s_idle_streak = 0;
            s_idle_quantum = 0;
            s_idle_progress_reg = -2;
            s_idle_progress_delta = 0;
        }
        idle_snapshot_regs(cpu);
    } else if (check_pc == s_idle_pc &&
               g_guest_store_count == s_idle_last_stores &&
               g_mmio_access_count == s_idle_last_mmio) {
        /* Second consecutive observation of this PC with no stores/MMIO —
         * take the baseline snapshot; the next hit can compare. Defers the
         * 31-GPR copy away from branchy VLC/decode edges that change resume
         * PC every check (pure host cost, never skipped). */
        idle_snapshot_regs(cpu);
        s_idle_have_snap = 1;
        s_idle_streak = 0;
        s_idle_quantum = 0;
        s_idle_progress_reg = -2;
        s_idle_progress_delta = 0;
    } else {
        s_idle_pc = check_pc;
        s_idle_streak = 0;
        s_idle_quantum = 0;
        s_idle_progress_reg = -2;
        s_idle_progress_delta = 0;
        s_idle_have_snap = 0;
    }
    s_idle_last_cycle  = cyc;
    s_idle_last_stores = g_guest_store_count;
    s_idle_last_mmio   = g_mmio_access_count;

    if (s_idle_streak < IDLE_STREAK_MIN) return;
    uint32_t q = s_idle_quantum;
    uint32_t dist = devices_cycles_to_next_idle_event();
    if (dist <= q) return;               /* one real iteration reaches it */
    uint32_t k = (dist + q - 1u) / q;    /* first check boundary >= event */
    if (s_idle_progress_reg > 0) {
        uint32_t value = cpu->gpr[s_idle_progress_reg];
        uint32_t max_k = value > 1u ? value - 1u : 0u;
        if (k > max_k) k = max_k;
        if (k == 0) return;
    }
    uint64_t skip = (uint64_t)k * q;
    if (skip > IDLE_SKIP_MAX_CYCLES) return;

    if (s_idle_progress_reg > 0)
        cpu->gpr[s_idle_progress_reg] -= k;

    g_idle_skip_count++;
    g_idle_skip_cycles += skip;
    g_idle_skip_last_pc = check_pc;
    g_idle_skip_last_quantum = q;
    psx_advance_cycles((uint32_t)skip);  /* replays device events exactly */

    /* Device servicing may have written RAM / raised I_STAT — the caller
     * (psx_check_interrupts) evaluates deliverability right after this with
     * the post-skip state. Re-detect from scratch either way. */
    s_idle_streak = 0;
    idle_snapshot_regs(cpu);
    s_idle_have_snap = 1;
    s_idle_last_cycle  = psx_cycle_count;
    s_idle_last_stores = g_guest_store_count;
    s_idle_last_mmio   = g_mmio_access_count;
#endif
}

/* Save-state restore: psx_cycle_count was just overwritten from the snapshot, so
 * the deadline-model bookkeeping (synced position + next deadline) is stale and
 * would try to replay a bogus gap. Re-anchor devices at the restored cycle and
 * force a fresh deadline on the next charge. */
void psx_cycles_resync_after_restore(CPUState *cpu) {
    if (g_psx_cyc_local_acc) *g_psx_cyc_local_acc = 0;
    g_psx_cyc_local_acc    = NULL;
    g_psx_cyc_batch        = 0;
    g_psx_cyc_batch_limit  = 0;
    g_psx_cyc_bb_defer     = 0;
    s_devices_synced_cycle = psx_cycle_count;
    psx_next_service_cycle = 0;   /* recompute on next charge */
    psx_in_device_service  = 0;
    /* Idle-skip detector latches absolute cycle/store counters from the
     * pre-load timeline; drop them so a rewound clock cannot false-train. */
    s_idle_pc = 0;
    s_idle_streak = 0;
    s_idle_have_snap = 0;
    s_idle_progress_reg = -2;
    s_idle_last_cycle = psx_cycle_count;
    /* v8 restores guest timing and pending load writeback from the snapshot.
     * They belong to the restored clock and must survive host resynchronization. */
    (void)cpu;
    g_psx_cycle_fast_limit = 0;
    /* Entry-poll %%64 stride + 4096-insn pump gap are host-only; reset so
     * both peers take the first post-load dirty wait IRQ on the same phase. */
    {
        extern void dirty_ram_irq_ambient_resync_after_restore(void);
        dirty_ram_irq_ambient_resync_after_restore();
    }
}

void psx_cycles_reset_for_boot(void) {
    if (g_psx_cyc_local_acc) *g_psx_cyc_local_acc = 0;
    g_psx_cyc_local_acc    = NULL;
    g_psx_cyc_batch        = 0;
    g_psx_cyc_batch_limit  = 0;
    g_psx_cyc_bb_defer     = 0;
    psx_cycle_count        = 0;
    s_devices_synced_cycle = 0;
    s_next_service_cycle   = 0;
    s_in_device_service    = 0;
    g_psx_cycle_fast_limit = 0;
    s_next_watchdog        = 0;
    s_next_pc_sample       = 0;
}

/* ---- Mult/div completion-stall: deferred HI/LO deadlines -----------------
 *
 * MULT/MULTU/DIV/DIVU do not stall when issued. They set a completion
 * deadline (muldiv_ts_done) = published guest time + latency. MFHI/MFLO call
 * psx_muldiv_stall, which waits for that deadline; instructions between the
 * operation and the read absorb the latency.
 *
 * Latencies are not documented in PSX-SPX or No$PSX. They are oracle
 * observations: fixture set F1-F4-muldiv-gte, TSV sha256 5eb5d9959673a858...,
 * receipt set sha256 8b43a3fe95c8470d... (Z:/Share/psxrecomp/evidence/T172/
 * clean-rewrite-fixtures-20260926). The fixture reports the extra cycles an
 * MFLO placed directly after the operation takes; the latency passed here is
 * that stall minus one, the same convention test_muldiv_deferred's measured
 * MULT/MFLO sequence uses (latency 7 for the 8-cycle case).
 *
 * MULT/MULTU [ORACLE FIXTURE F1]: the stall depends only on the first operand
 * (rs), by its count of leading zero bits (for MULT, of the value with the
 * sign folded away). No dependence on rt was observed.
 *   leading zeros  0-11 -> stall 15 -> latency 14
 *   leading zeros 12-20 -> stall 11 -> latency 10
 *   leading zeros 21-32 -> stall  8 -> latency  7
 * DIV/DIVU [ORACLE FIXTURE F2]: stall 38 -> latency 37 for every operand pair
 * observed, including divide by zero and 0x80000000 / -1. This value is
 * PSX_DIV_LATENCY (cpu_state.h); the recompilers emit it as a literal and
 * test_muldiv_latency_fixture checks that the literal matches. */

#define PSX_MULT_LAT_WIDE   14u  /* [ORACLE FIXTURE F1] leading zeros 0-11  */
#define PSX_MULT_LAT_MID    10u  /* [ORACLE FIXTURE F1] leading zeros 12-20 */
#define PSX_MULT_LAT_NARROW  7u  /* [ORACLE FIXTURE F1] leading zeros 21-32 */

static inline uint32_t psx_leading_zeros32(uint32_t v)
{
    if (v == 0) return 32;
#if defined(_MSC_VER)
    unsigned long msb;
    _BitScanReverse(&msb, v);
    return 31u - (uint32_t)msb;
#else
    return (uint32_t)__builtin_clz(v);
#endif
}

static inline uint32_t psx_mult_latency_from_zeros(uint32_t zeros)
{
    if (zeros <= 11) return PSX_MULT_LAT_WIDE;
    if (zeros <= 20) return PSX_MULT_LAT_MID;
    return PSX_MULT_LAT_NARROW;
}

uint32_t psx_mult_latency_s(uint32_t rs) {  /* MULT: fold the sign away first */
    uint32_t folded = (rs & 0x80000000u) ? ~rs : rs;
    return psx_mult_latency_from_zeros(psx_leading_zeros32(folded));
}
uint32_t psx_mult_latency_u(uint32_t rs) {  /* MULTU */
    return psx_mult_latency_from_zeros(psx_leading_zeros32(rs));
}

/* Arm the HI/LO deadline from the published guest time. Pending batched
 * cycles are published first so the deadline counts from the real issue time. */
void psx_muldiv_set(CPUState* cpu, uint32_t latency) {
    psx_cyc_batch_flush();
    cpu->muldiv_ts_done = psx_cycle_count + latency;
}

/* MFHI/MFLO: wait for the HI/LO deadline. [ORACLE FIXTURE F5] (fixture set
 * F5-mflo-giveback, TSV sha256 83fb4efa6c7bf97f..., 96 cases), cross-checked
 * against F1/F2; replayed through this file's timing primitives, the rule
 * below reproduces every warm row of F5 (2288) and F1/F2 (8697):
 *  - with two or more cycles left, the read waits until the deadline;
 *  - with one cycle or less left, it does not wait at all;
 *  - the wait overlaps the most recent load's outstanding give-back, so the
 *    give-back left for later instructions shrinks by the cycles waited. */
void psx_muldiv_stall(CPUState* cpu) {
    psx_cyc_batch_flush();
    if (cpu->muldiv_ts_done <= psx_cycle_count + 1) return;
    uint32_t stall = (uint32_t)(cpu->muldiv_ts_done - psx_cycle_count);
    uint8_t* give = &cpu->read_absorb[cpu->read_absorb_which];
    *give = stall >= *give ? 0 : (uint8_t)(*give - stall);
    psx_advance_cycles(stall);
}

/* ---- GTE completion-stall: deferred COP2 deadline --------------------------
 *
 * A GTE command sets gte_ts_done = published time + its added latency, after
 * first waiting for any earlier command still running. Every COP2 register
 * access (MFC2, CFC2, MTC2, CTC2, LWC2, SWC2) waits for gte_ts_done. MFC2 and
 * CFC2 also hand the actual stall to the delayed-load model as the target
 * register's give-back. Stalls only advance guest cycles.
 *
 * Latency = command cycle count - 1 (the issue cycle is charged by the
 * caller). Counts are [DOC] PSX-SPX a253f078
 * docs/geometrytransformationenginegte.md, "GTE Coordinate / General Purpose /
 * Color Calculation Commands" headings, confirmed by fixture F3 (same set as
 * above), except where marked [ORACLE FIXTURE F3]. */
static const uint8_t PSX_GTE_LATENCY[64] = {
    [0x00] = 14, /* undefined: [ORACLE FIXTURE F3] 15 cycles                  */
    [0x01] = 14, /* RTPS  15 [DOC]                                            */
    [0x06] =  7, /* NCLIP  8 [DOC]                                            */
    [0x0C] =  5, /* OP     6 [DOC]                                            */
    [0x10] =  7, /* DPCS   8 [DOC]                                            */
    [0x11] =  7, /* INTPL  8 [DOC]                                            */
    [0x12] =  7, /* MVMVA  8 [DOC]                                            */
    [0x13] = 18, /* NCDS  19 [DOC]                                            */
    [0x14] = 12, /* CDP   13 [DOC]                                            */
    [0x16] = 43, /* NCDT  44 [DOC]                                            */
    [0x1A] =  7, /* undefined: [ORACLE FIXTURE F3] 8 cycles                   */
    [0x1B] = 16, /* NCCS  17 [DOC]                                            */
    [0x1C] = 10, /* CC    11 [DOC]                                            */
    [0x1E] = 13, /* NCS   14 [DOC]                                            */
    [0x20] = 29, /* NCT   30 [DOC]                                            */
    [0x28] =  4, /* SQR    5 [DOC]                                            */
    [0x29] =  7, /* DCPL   8 [DOC]                                            */
    [0x2A] = 16, /* DPCT  17 [DOC]                                            */
    [0x2D] =  4, /* AVSZ3  5 [DOC]                                            */
    [0x2E] =  4, /* AVSZ4: [ORACLE FIXTURE F3] 5 cycles; PSX-SPX lists 6      */
    [0x30] = 22, /* RTPT  23 [DOC]                                            */
    [0x3D] =  4, /* GPF    5 [DOC]                                            */
    [0x3E] =  4, /* GPL    5 [DOC]                                            */
    [0x3F] = 38, /* NCCT  39 [DOC]                                            */
    /* All other command numbers: no added latency [ORACLE FIXTURE F3]. */
};

uint32_t psx_gte_cmd_latency(uint32_t cmd) {
    return PSX_GTE_LATENCY[cmd & 0x3Fu];
}

/* Advance guest time to the GTE deadline if it lies ahead; returns the stall. */
static inline uint32_t psx_gte_wait(const CPUState* cpu) {
    psx_cyc_batch_flush();
    if (psx_cycle_count >= cpu->gte_ts_done) return 0;
    uint32_t stall = (uint32_t)(cpu->gte_ts_done - psx_cycle_count);
    psx_advance_cycles(stall);
    return stall;
}

void psx_gte_set(CPUState* cpu, uint32_t latency) {
    (void)psx_gte_wait(cpu);        /* the previous command finishes first */
    cpu->gte_ts_done = psx_cycle_count + latency;
}

void psx_gte_stall(CPUState* cpu) {
    (void)psx_gte_wait(cpu);
}

/* MFC2/CFC2: the stall becomes the pending load's give-back for rt. */
void psx_gte_read(CPUState* cpu, uint32_t rt) {
    cpu->ld_absorb = psx_gte_wait(cpu);
    cpu->ld_which_t = (uint8_t)rt;
}
