#include "psx_netplay_rb.h"

#if !defined(PSX_HAS_RECOMP_NET)
void psx_netplay_rb_bind(const PsxNetplayRbBindings *b) { (void)b; }
void psx_netplay_rb_start(void) {}
void psx_netplay_rb_shutdown(void) {}
void psx_netplay_rb_poll(struct CPUState *cpu, uint32_t resume_pc)
{
    (void)cpu;
    (void)resume_pc;
}
void psx_netplay_rb_flush_resume(void) {}
int psx_netplay_rb_top_level_resume_active(void) { return 0; }
int psx_netplay_rb_recover_null_pc(struct CPUState *cpu, uint32_t *out_pc)
{
    (void)cpu;
    (void)out_pc;
    return 0;
}
void psx_netplay_rb_request_snap(uint32_t tick) { (void)tick; }
int psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot)
{
    (void)mismatch_tick;
    (void)slot;
    return 0;
}
int psx_netplay_rb_tip_extend(uint32_t mismatch_tick, int slot)
{
    (void)mismatch_tick;
    (void)slot;
    return 0;
}
int psx_netplay_rb_rewind_suppressed(void) { return 0; }
int psx_netplay_rb_fmv_defer_rewind(void) { return 0; }
int psx_netplay_rb_fmv_media_active(void) { return 0; }
int psx_netplay_rb_lockstep_no_invent(void) { return 0; }
void psx_netplay_rb_poll_replay_stall(void) {}
int psx_netplay_rb_take_promote_sweep(void) { return 0; }
void psx_netplay_rb_pump(void) {}
int psx_netplay_rb_active(void) { return 0; }
int psx_netplay_rb_is_resimulating(void) { return 0; }
int psx_netplay_rb_tip_holding(void) { return 0; }
int psx_netplay_rb_abort_resim_core_mismatch(uint32_t tick, uint32_t local_core,
                                             uint32_t peer_core)
{
    (void)tick;
    (void)local_core;
    (void)peer_core;
    return 0;
}
int psx_netplay_rb_load_pending(void) { return 0; }
int psx_netplay_rb_try_admit(void) { return 0; }
void psx_netplay_rb_finish_frame(void) {}
void psx_netplay_rb_cpu_for_present_digest(struct CPUState *out,
                                           const struct CPUState *in)
{
    if (!out || !in)
        return;
    *out = *in;
    out->pc = 0;
}
uint32_t psx_netplay_rb_episode_count(void) { return 0; }
int psx_netplay_rb_phase(void) { return 0; }
uint32_t psx_netplay_rb_snap_count(void) { return 0; }
#else

#include "netplay_hash_confirm.h"
#include "netplay_input_hist.h"
#include "netplay_snap_ring.h"
#include "netplay_state_digest.h"
#include "boot_state.h"
#include "cdrom.h"
#include "cpu_state.h"
#include "crc32.h"
#include "gpu.h"
#include "interrupts.h"
#include "mdec.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include "savestate.h"
#include "spu.h"

#include "recomp_net/recomp_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PsxNetplayRbBindings g_b;
static int g_bound;
static RNetRbSession *g_rb;
static NetplaySnapRing *g_snaps;
static uint32_t g_epoch;
static uint32_t g_episode_count;
static int g_pending_save_valid;
static uint32_t g_pending_save_tick;
static int g_pending_load_valid;
static uint32_t g_pending_load_tick;
/* Set after apply-from-pump/admit; flushed via psx_netplay_rb_flush_resume. */
static int g_pending_resume_valid;
static uint32_t g_pending_resume_pc;
/* 1 only after a successful ring load for this episode — blocks baseline/replay
 * until restore (peer BASELINE can arrive while we are still sealing). */
static int g_episode_snap_applied;

/* Live-path snap every N ticks (raw boot_state ~3.5MB, no zlib). Resim still
 * snaps every tick so episode baselines stay dense.
 * Override: PSX_NET_SNAP_INTERVAL (default 16 — was 8; live memcpy was a
 * major FPS tax stacked on per-frame RAM digests). */
static uint32_t snap_interval(void)
{
    static int latched;
    static uint32_t iv = 16u;
    if (!latched) {
        const char *e = getenv("PSX_NET_SNAP_INTERVAL");
        if (e && e[0]) {
            unsigned v = 0;
            if (sscanf(e, "%u", &v) == 1 && v >= 1u && v <= 32u)
                iv = v;
        }
        latched = 1;
    }
    return iv;
}
static int g_local_baseline_sent;
static uint32_t g_local_baseline_digest;
static uint32_t g_local_baseline_av;
static uint32_t g_local_baseline_aux; /* dig_c = ext = crc(aux,cd,spad,dma,sio) */
static int g_peer_baseline_ok;
static uint32_t g_peer_baseline_digest;
static uint32_t g_peer_baseline_av;
static uint32_t g_peer_baseline_aux;
static int g_peer_baseline_ready; /* dig_a flag: follower ACK after it has our baseline */
static int g_local_baseline_ready_sent;
static int g_local_post_sent;
static int g_peer_post_ok;
static uint32_t g_peer_post_digest;
static uint8_t g_peer_post_match;
static uint32_t g_post_digest;
static uint32_t g_post_av;
static uint32_t g_peer_post_av;
static int g_needs_advance;
static int g_seal_export_logged;
static int g_baseline_rexmit_logged;
static int g_post_rexmit_logged;
static uint64_t g_seal_wait_ms; /* CLOCK_MONOTONIC ms when SealInputs began */
static uint64_t g_verify_wait_ms;
static uint64_t g_replay_progress_ms; /* last arm / finish_frame */
static uint32_t g_last_good_bb_pc; /* sticky BB-edge resume for snaps / digest */
static int g_follow_nack_pending;
static uint32_t g_follow_nack_epoch;
static uint32_t g_follow_nack_mismatch;
static uint32_t g_follow_nack_load;
static uint32_t g_follow_nack_target;
static int g_follow_nack_slot;
static int g_follow_nack_sends;
/* BASELINE that arrived before follow/begin (TURN race) — applied on episode open. */
static int g_stash_bl_valid;
static uint32_t g_stash_bl_epoch;
static uint32_t g_stash_bl_load;
static uint32_t g_stash_bl_dig_m;
static uint32_t g_stash_bl_dig_a;
static uint32_t g_stash_bl_dig_b;
static uint32_t g_stash_bl_dig_c;
static int g_stash_bl_logged;
static uint64_t g_baseline_handshake_ms; /* when local baseline first sent */
static uint64_t g_last_baseline_burst_ms;
static int g_ready_timeout_logged;
static int g_wait_peer_bl_logged;
/* Last committed target — Live realign after POST/baseline abort. */
static int g_agreed_valid;
static uint32_t g_agreed_through;
/* Load tick of the last committed episode. Both peers replayed
 * [span_lo+1 .. agreed_through] and resim saves a snap at every replayed tick,
 * so any ring snap inside that span is guaranteed to exist on the peer too. */
static uint32_t g_agreed_span_lo;
/* 1: pending load is a Live realign (not an episode baseline). */
static int g_live_realign_pending;
static uint32_t g_episode_load_tick; /* captured for post-diverge realign */
static int g_episode_baseline_matched;
/* Frozen copy of the episode baseline snap — resim must not clobber it in the
 * ring (finish_frame request_snap overwrote load_tick and poisoned realign). */
static uint8_t *g_pin_data;
static size_t g_pin_size;
static uint32_t g_pin_tick;
static int g_pin_valid;
/* After abort/realign only: refuse new episodes for N sim ticks (promote wire).
 * Successful commit does NOT arm this — late wire must open another episode. */
static uint32_t g_rewind_cooldown_until;
static int g_promote_sweep;
static uint32_t g_last_begin_mismatch = 0xffffffffu;
/* digest_a bit: follower has peer baseline + is entering / in Replay */
#define RB_BL_FLAG_READY 1u
#define RB_SEAL_TIMEOUT_MS 4000u
#define RB_VERIFY_TIMEOUT_MS 4000u
#define RB_REPLAY_STALL_MS 5000u
#define RB_FOLLOW_NACK_REXMIT 24
#define RB_BASELINE_BURST 8
#define RB_BASELINE_BURST_MS 40u
/* Initiator waits for follower ready-ACK; do NOT solo-enter Replay. */
#define RB_READY_TIMEOUT_MS 4000u
/* Failed episode (resim/baseline/post): calm from *live* sim before realign. */
#define RB_ABORT_COOLDOWN_TICKS 60u
/* Repeated abort streak — longer promote-only (char-select storm). */
#define RB_STORM_COOLDOWN_TICKS 120u
/* Keep deferring a few frames after MDEC goes quiet (cutover / credits). */
#define RB_FMV_MDEC_HYSTERESIS 8u
/* After leaving FMV: defer rewind briefly so cutover heals. */
#define RB_FMV_SETTLE_TICKS 24u
/* Post-FMV no-invent lockstep (title Start / skip safe). Floor keeps invent
 * off for at least MIN even when digests already agree; if cores have not
 * matched for CONFIRM contiguous ticks by then, hold until they do or MAX
 * (cross-game cutover — MotK was healthy at ~90, slower titles need gate). */
#define RB_FMV_LOCKSTEP_MIN 90u
#define RB_FMV_LOCKSTEP_MAX 180u
#define RB_FMV_LOCKSTEP_CONFIRM 16u
/* Baseline mismatch with no agreed tip — don't reopen every few ticks. */
#define RB_BASELINE_MISMATCH_COOLDOWN_TICKS 90u

static int g_was_in_fmv;
static uint32_t g_fmv_settle_until;
static uint32_t g_fmv_lockstep_until;
static uint32_t g_fmv_media_end_sim;
static uint32_t g_fmv_core_match_streak;
static int g_fmv_lockstep_released; /* sticky: never re-lock after invent on */
/* Inclusive last tick both peers dense-snapped (media/lockstep/+tip). */
static uint32_t g_fmv_dense_through;
static uint32_t g_bl_mismatch_streak;
static uint32_t g_tip_hold_until;
/* Mid-span tip-extend rereplay: reload snap without replacing episode pin. */
static int g_tip_extend_rereplay;

static RNetSession *sess(void); /* fwd — used by FMV lockstep window */
static void commit_episode(void); /* fwd — legacy immediate commit */
static void enter_tip_hold(uint32_t target);
static void enter_verify_at_tip(uint32_t done);
static void finalize_tip_hold(void);
static void poll_tip_hold_finalize(void);

static int rb_fmv_media_active(void)
{
    if (gpu_display_is_depth24())
        return 1;
    if (mdec_recently_active(RB_FMV_MDEC_HYSTERESIS))
        return 1;
    return 0;
}

/* After media: grow/shrink lockstep_until from hash_confirm agreement. */
static void rb_fmv_update_lockstep_gate(uint32_t sim)
{
    int matched = 0;
    uint32_t cap;
    uint32_t prev_until;
    if (!g_fmv_media_end_sim || rb_fmv_media_active() || g_fmv_lockstep_released)
        return;

    if (g_b.hc && !netplay_hc_peek_mismatch(g_b.hc, NULL, NULL, NULL)) {
        uint32_t need = (sim > 0u) ? (sim - 1u) : 0u;
        if (netplay_hc_confirm_through(g_b.hc, need))
            matched = 1;
    }
    if (matched)
        g_fmv_core_match_streak++;
    else
        g_fmv_core_match_streak = 0;

    if (sim < g_fmv_media_end_sim + RB_FMV_LOCKSTEP_MIN)
        return; /* accumulate streak; invent stays off through MIN */

    prev_until = g_fmv_lockstep_until;
    cap = g_fmv_media_end_sim + RB_FMV_LOCKSTEP_MAX;
    if (sim >= cap || g_fmv_core_match_streak >= RB_FMV_LOCKSTEP_CONFIRM) {
        if (g_fmv_lockstep_until > sim)
            g_fmv_lockstep_until = sim;
        g_fmv_lockstep_released = 1;
        fprintf(stderr,
                "psxrecomp: rb FMV lockstep RELEASE sim=%u streak=%u "
                "cap=%u (invent on%s)\n",
                (unsigned)sim, (unsigned)g_fmv_core_match_streak,
                (unsigned)cap,
                (sim >= cap) ? "; MAX" : "; cores matched");
        fflush(stderr);
        (void)prev_until;
        return;
    }
    /* Past MIN, cores not confirmed — hold invent until match or MAX. */
    {
        uint32_t until = sim + 1u;
        if (until > cap)
            until = cap;
        if (until > g_fmv_lockstep_until)
            g_fmv_lockstep_until = until;
    }
    if (g_fmv_lockstep_until > prev_until) {
        static uint32_t s_ext_log;
        if (s_ext_log != sim) {
            fprintf(stderr,
                    "psxrecomp: rb FMV lockstep EXTEND until=%u sim=%u "
                    "streak=%u (waiting core confirm)\n",
                    (unsigned)g_fmv_lockstep_until, (unsigned)sim,
                    (unsigned)g_fmv_core_match_streak);
            fflush(stderr);
            s_ext_log = sim;
        }
    }
}

/* Track settle window after FMV ends. */
static void rb_fmv_tick_settle(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    int media = rb_fmv_media_active();

    if (media) {
        if (!g_was_in_fmv) {
            g_was_in_fmv = 1;
            fprintf(stderr,
                    "psxrecomp: rb FMV rewind-defer ON (depth24/mdec; no invent)\n");
            fflush(stderr);
        }
        return;
    }
    if (g_was_in_fmv) {
        g_was_in_fmv = 0;
        g_fmv_media_end_sim = sim;
        g_fmv_core_match_streak = 0;
        g_fmv_lockstep_released = 0;
        g_fmv_settle_until = sim + RB_FMV_SETTLE_TICKS;
        g_fmv_lockstep_until = sim + RB_FMV_LOCKSTEP_MIN;
        /* Live FRAME_COMMITs were skipped during depth24 — waive the gap so
         * hash_confirm can advance again once digests resume. */
        if (g_b.hc)
            netplay_hc_prime_after(g_b.hc, sim);
        fprintf(stderr,
                "psxrecomp: rb FMV settle until sim=%u lockstep min=%u "
                "max=%u confirm=%u (no invent; hc primed)\n",
                (unsigned)g_fmv_settle_until,
                (unsigned)g_fmv_lockstep_until,
                (unsigned)(sim + RB_FMV_LOCKSTEP_MAX),
                (unsigned)RB_FMV_LOCKSTEP_CONFIRM);
        fflush(stderr);
    }
    rb_fmv_update_lockstep_gate(sim);
}

/* Defer rewind during movie + short settle. */
static int rb_in_fmv_defer_rewind_window(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    rb_fmv_tick_settle();
    if (rb_fmv_media_active())
        return 1;
    return sim < g_fmv_settle_until;
}

/* Media + post-FMV lockstep: admit waits for remote wire (title Start safe). */
static int rb_in_fmv_lockstep_window(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    rb_fmv_tick_settle();
    if (rb_fmv_media_active())
        return 1;
    return sim < g_fmv_lockstep_until;
}

static void arm_rewind_cooldown_ticks(uint32_t sim, uint32_t ticks, const char *why)
{
    uint32_t until = sim + ticks;
    if (until > g_rewind_cooldown_until)
        g_rewind_cooldown_until = until;
    g_promote_sweep = 1;
    fprintf(stderr, "psxrecomp: rb rewind cooldown until sim=%u (%s)\n",
            (unsigned)g_rewind_cooldown_until, why ? why : "?");
    fflush(stderr);
}

#if defined(PSX_HAS_GAME_DISPATCH)
extern int psx_game_is_function_entry(uint32_t addr);
#endif

static RNetSession *sess(void)
{
    return (g_bound && g_b.session) ? *g_b.session : NULL;
}

static CPUState *cpu(void)
{
    return (g_bound && g_b.cpu) ? *g_b.cpu : NULL;
}

/* psx_is_dispatchable only rejects 0 / sentinel — 0xB0 etc. still pass and
 * produce lethal flush_resume targets. Require aligned BIOS or main RAM. */
static int rb_resume_pc_ok(uint32_t pc)
{
    uint32_t phys;
    if (!psx_is_dispatchable(pc))
        return 0;
    if ((pc & 3u) != 0u)
        return 0;
    /* Exception / reset vectors are not safe episode resume points. */
    if (pc == 0x80000080u || pc == 0xbfc00180u || pc == 0x80000000u)
        return 0;
    if ((pc & 0xfff00000u) == 0xbfc00000u)
        return 1; /* BIOS ROM */
    phys = pc & 0x1fffffffu;
    /* 2MB main RAM; skip the low scratch page (catches 0xB0 / 0x800000B0). */
    if (phys >= 0x1000u && phys < 0x200000u)
        return 1;
    return 0;
}

/* MotK title/menu wait ping-pong. Opposite edges with matched clocks fork
 * dig_cpu (v0 countdown vs 1) while FRAME_COMMIT clears PC and still matches.
 * Always store the post-present edge so both peers pin the same resume. */
#define RB_MOTK_WAIT_A 0x8006CD54u
#define RB_MOTK_WAIT_B 0x8006CDA0u

static uint32_t rb_canonicalize_resume_pc(uint32_t pc)
{
    if (pc == RB_MOTK_WAIT_A || pc == RB_MOTK_WAIT_B)
        return RB_MOTK_WAIT_B;
    return pc;
}

/* Prefer BB-edge / IRQ check PCs — finish_frame often sees cpu->pc==0 while the
 * guest is parked in the vblank present path. IRQ/sticky/$ra first: bare
 * function-entry snaps picked short jr-$ra leaves (0x8006A9F8) whose top-level
 * flush_resume re-dispatch published pc=0 (cps exit: final_ra=0x8006CCA0). */
static uint32_t pick_snap_resume_pc(const CPUState *c, uint32_t hint)
{
    const uint32_t cands[6] = {
        psx_last_irq_check_pc(),
        g_last_good_bb_pc,
        psx_compiled_irq_resume_pc(),
        c ? c->gpr[31] : 0u,
        hint,
        c ? c->pc : 0u,
    };
    int i;
    for (i = 0; i < 6; ++i) {
        if (rb_resume_pc_ok(cands[i]))
            return rb_canonicalize_resume_pc(cands[i]);
    }
    return 0;
}

static void note_good_bb_pc(uint32_t pc)
{
    if (rb_resume_pc_ok(pc))
        g_last_good_bb_pc = pc;
}

void psx_netplay_rb_cpu_for_present_digest(struct CPUState *out,
                                           const struct CPUState *in)
{
    if (!out || !in)
        return;
    *out = *in;
    /* Drop host-parking / BB-edge PC noise from present-edge digests. */
    out->pc = 0;
}

/* Split dig_cpu so logs tell PC-only forks from true GPR/COP0/GTE forks. */
static void log_cpu_digest_split(const CPUState *raw, const char *tag)
{
    uint32_t gpr = 0xFFFFFFFFu;
    uint32_t hil = 0xFFFFFFFFu;
    uint32_t c0 = 0xFFFFFFFFu;
    uint32_t gte = 0xFFFFFFFFu;
    int dump_regs;
    int i;
    if (!raw)
        return;
    gpr = crc32_update(gpr, (const uint8_t *)raw->gpr, sizeof(raw->gpr));
    hil = crc32_update(hil, (const uint8_t *)&raw->hi, sizeof(raw->hi));
    hil = crc32_update(hil, (const uint8_t *)&raw->lo, sizeof(raw->lo));
    c0 = crc32_update(c0, (const uint8_t *)&raw->cop0[12], sizeof(uint32_t));
    c0 = crc32_update(c0, (const uint8_t *)&raw->cop0[13], sizeof(uint32_t));
    c0 = crc32_update(c0, (const uint8_t *)&raw->cop0[14], sizeof(uint32_t));
    gte = crc32_update(gte, (const uint8_t *)raw->gte_data, sizeof(raw->gte_data));
    gte = crc32_update(gte, (const uint8_t *)raw->gte_ctrl, sizeof(raw->gte_ctrl));
    fprintf(stderr,
            "psxrecomp: rb cpu-split %s raw_pc=%08x sticky=%08x gpr=%08x "
            "hi_lo=%08x cop0=%08x gte=%08x\n",
            tag ? tag : "?", (unsigned)raw->pc, (unsigned)g_last_good_bb_pc,
            (unsigned)(gpr ^ 0xFFFFFFFFu), (unsigned)(hil ^ 0xFFFFFFFFu),
            (unsigned)(c0 ^ 0xFFFFFFFFu), (unsigned)(gte ^ 0xFFFFFFFFu));
    /* Full GPR words on fin/abort — peers can diff which regs forked. */
    dump_regs = tag && (strcmp(tag, "fin") == 0 || strcmp(tag, "abort") == 0 ||
                        strcmp(tag, "post") == 0);
    if (dump_regs) {
        fprintf(stderr, "psxrecomp: rb gpr-dump %s", tag);
        for (i = 0; i < 32; i++)
            fprintf(stderr, " r%02d=%08x", i, (unsigned)raw->gpr[i]);
        fprintf(stderr, "\n");
        /* Last VBLANK irq restore — peers diff restored/v0 on r2-only forks. */
        interrupts_log_last_vblank_irqctx(tag);
    }
    fflush(stderr);
}

static void clear_episode_wire_state(void)
{
    g_local_baseline_sent = 0;
    g_local_baseline_digest = 0;
    g_local_baseline_av = 0;
    g_local_baseline_aux = 0;
    g_peer_baseline_ok = 0;
    g_peer_baseline_digest = 0;
    g_peer_baseline_av = 0;
    g_peer_baseline_aux = 0;
    g_peer_baseline_ready = 0;
    g_local_baseline_ready_sent = 0;
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
    g_post_av = 0;
    g_peer_post_av = 0;
    g_baseline_rexmit_logged = 0;
    g_post_rexmit_logged = 0;
    g_seal_wait_ms = 0;
    g_verify_wait_ms = 0;
    g_replay_progress_ms = 0;
    g_baseline_handshake_ms = 0;
    g_last_baseline_burst_ms = 0;
    g_ready_timeout_logged = 0;
    g_wait_peer_bl_logged = 0;
}

static void clear_baseline_pin(void)
{
    free(g_pin_data);
    g_pin_data = NULL;
    g_pin_size = 0;
    g_pin_tick = 0;
    g_pin_valid = 0;
}

static void pin_baseline_from_ring(uint32_t tick)
{
    size_t sz = 0;
    const uint8_t *p;
    uint8_t *copy;
    if (!g_snaps)
        return;
    p = netplay_snap_ring_peek(g_snaps, tick, &sz);
    if (!p || sz == 0)
        return;
    copy = (uint8_t *)malloc(sz);
    if (!copy)
        return;
    memcpy(copy, p, sz);
    clear_baseline_pin();
    g_pin_data = copy;
    g_pin_size = sz;
    g_pin_tick = tick;
    g_pin_valid = 1;
    fprintf(stderr, "psxrecomp: rb baseline pinned tick=%u bytes=%zu\n",
            (unsigned)tick, sz);
    fflush(stderr);
}

static void schedule_live_realign(uint32_t tick, const char *why)
{
    RNetSession *s = sess();
    int have_pin = g_pin_valid && g_pin_tick == tick && g_pin_data && g_pin_size;
    if (!have_pin && (!g_snaps || !netplay_snap_ring_has(g_snaps, tick))) {
        fprintf(stderr,
                "psxrecomp: rb realign SKIPPED tick=%u — snap missing (%s)\n",
                (unsigned)tick, why ? why : "?");
        fflush(stderr);
        return;
    }
    g_live_realign_pending = 1;
    g_pending_load_tick = tick;
    g_pending_load_valid = 1;
    g_pending_resume_valid = 0;
    /* Snap is post-tick state; continue Live at tick+1 (same as episode commit). */
    if (s)
        rnet_session_set_sim_tick(s, tick + 1u);
    fprintf(stderr, "psxrecomp: rb realign queued tick=%u → sim=%u pin=%d (%s)\n",
            (unsigned)tick, (unsigned)(tick + 1u), have_pin, why ? why : "?");
    fflush(stderr);
}

static void abort_episode(const char *why)
{
    fprintf(stderr, "psxrecomp: rb episode ABORT — %s\n", why ? why : "?");
    fflush(stderr);
    /* Preserve a queued Live realign load; clear only episode baseline loads. */
    if (!g_live_realign_pending)
        g_pending_load_valid = 0;
    g_pending_resume_valid = 0;
    g_episode_snap_applied = 0;
    g_needs_advance = 0;
    g_episode_baseline_matched = 0;
    g_tip_hold_until = 0;
    g_tip_extend_rereplay = 0;
    psx_scheduler_top_level_resume_clear();
    clear_episode_wire_state();
    if (g_rb && rnet_rb_is_active(g_rb)) {
        rnet_rb_set_phase(g_rb, nRNetRbPhaseAbort);
        rnet_rb_session_reset(g_rb);
    }
}

int psx_netplay_rb_top_level_resume_active(void)
{
    /* Shared with savestate/selfcheck — armed inside psx_scheduler_resume_at. */
    return psx_scheduler_top_level_resume_active();
}

/* Top-level flush_resume has no native chain under the resume PC. A leaf that
 * returns (or sentinel "continue native") publishes pc=0 → GUEST_EXIT. Recover
 * at $ra / sticky BB when that is a safe re-entry. */
int psx_netplay_rb_recover_null_pc(struct CPUState *cpu_in, uint32_t *out_pc)
{
    uint32_t cand;
    int i;
    if (!psx_scheduler_top_level_resume_active() || !cpu_in || !out_pc)
        return 0;
    {
        const uint32_t cands[4] = {
            cpu_in->gpr[31],
            g_last_good_bb_pc,
            psx_last_irq_check_pc(),
            psx_compiled_irq_resume_pc(),
        };
        for (i = 0; i < 4; ++i) {
            cand = cands[i];
            if (!rb_resume_pc_ok(cand))
                continue;
            fprintf(stderr,
                    "psxrecomp: rb recover null-pc → 0x%08x (ra=0x%08x sticky=0x%08x)\n",
                    (unsigned)cand, (unsigned)cpu_in->gpr[31],
                    (unsigned)g_last_good_bb_pc);
            fflush(stderr);
            *out_pc = cand;
            note_good_bb_pc(cand);
            return 1;
        }
    }
    fprintf(stderr,
            "psxrecomp: rb recover null-pc FAILED ra=0x%08x sticky=0x%08x\n",
            (unsigned)cpu_in->gpr[31], (unsigned)g_last_good_bb_pc);
    fflush(stderr);
    psx_scheduler_top_level_resume_clear();
    return 0;
}

/* After a failed episode: rewind Live to a tip both peers already agreed on. */
static void abort_episode_realign(const char *why)
{
    RNetSession *s = sess();
    /* Capture LIVE sim BEFORE schedule_live_realign rewinds the clock.
     * Arming cooldown from the rewound tip let uncapped catch-up burn a
     * 12-tick window in ms → char-select episode storms (stale until < live). */
    uint32_t live_sim = s ? rnet_session_sim_tick(s) : 0u;
    uint32_t tick = 0;
    int have = 0;
    uint32_t cool_n;
    char cool_why[160];
    int hard_fail;

    /* Prefer the frozen pre-resim baseline — ring load_tick was often overwritten. */
    if (g_episode_baseline_matched && g_pin_valid) {
        tick = g_pin_tick;
        have = 1;
    } else if (g_episode_baseline_matched && g_snaps &&
               netplay_snap_ring_has(g_snaps, g_episode_load_tick)) {
        tick = g_episode_load_tick;
        have = 1;
    } else if (g_agreed_valid && g_snaps &&
               netplay_snap_ring_has(g_snaps, g_agreed_through)) {
        tick = g_agreed_through;
        have = 1;
    }
    abort_episode(why);
    if (have)
        schedule_live_realign(tick, why);

    hard_fail = !have ||
                (why && (strstr(why, "baseline") != NULL ||
                         strstr(why, "resim core") != NULL ||
                         strstr(why, "post ") != NULL ||
                         strstr(why, "desync") != NULL));
    if (hard_fail)
        g_bl_mismatch_streak++;
    else
        g_bl_mismatch_streak = 0;

    if (!have)
        cool_n = RB_BASELINE_MISMATCH_COOLDOWN_TICKS;
    else if (g_bl_mismatch_streak >= 2u)
        cool_n = RB_STORM_COOLDOWN_TICKS;
    else if (hard_fail)
        cool_n = RB_ABORT_COOLDOWN_TICKS;
    else
        cool_n = RB_ABORT_COOLDOWN_TICKS;

    snprintf(cool_why, sizeof(cool_why), "%s (live=%u streak=%u)",
             why ? why : "realign", (unsigned)live_sim,
             (unsigned)g_bl_mismatch_streak);
    arm_rewind_cooldown_ticks(live_sim, cool_n, cool_why);
}

static int host_save_state(void *ctx, uint32_t tick)
{
    CPUState *c = cpu();
    uint32_t pc;
    CPUState snap;
    (void)ctx;
    if (!g_snaps || !c || !g_b.bios_checksum || !g_b.entry_pc)
        return -1;
    pc = pick_snap_resume_pc(c, 0u);
    if (!pc)
        return -1;
    snap = *c;
    snap.pc = pc;
    return netplay_snap_ring_save(g_snaps, tick, &snap, *g_b.bios_checksum, *g_b.entry_pc)
               ? 0
               : -1;
}

static int host_load_state(void *ctx, uint32_t tick)
{
    (void)ctx;
    g_pending_load_tick = tick;
    g_pending_load_valid = 1;
    return 0;
}

static int host_advance_sim(void *ctx, uint32_t tick)
{
    (void)ctx;
    (void)tick;
    return 0; /* MotK advances via the live guest frame loop. */
}

static uint32_t host_state_digest(void *ctx, uint32_t tick, uint32_t partition)
{
    CPUState *c = cpu();
    (void)ctx;
    (void)tick;
    (void)partition;
    if (!c)
        return 0;
    /* Agree on CPU/RAM/IRQ/timers only — CDROM FSM still forks across peers
     * during MotK title resim (audit cd=); folding it aborted good corrections. */
    return netplay_core_digest(c);
}

static uint8_t host_hash_confirm_through(void *ctx, uint32_t tick)
{
    (void)ctx;
    if (!g_b.hc)
        return 0;
    return netplay_hc_confirm_through(g_b.hc, tick);
}

static uint8_t host_get_input_row(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out)
{
    (void)ctx;
    if (!g_b.ih || !out)
        return 0;
    if (netplay_ih_get(g_b.ih, (int)slot, tick, out))
        return 1u;
    /* Seal path requires is_valid=1 rows or the peer never completes SealInputs
     * (apply_peer_seal_rows ignores invalid). MotK: idle invent fills gaps —
     * hold-last reintroduced sticky D-pad into Replay seals. */
    return netplay_ih_invent_idle(g_b.ih, (int)slot, tick, out) ? 1u : 0u;
}

static uint8_t host_hash_confirm_promote(void *ctx)
{
    uint32_t *tickp = (uint32_t *)ctx;
    if (!tickp || !g_b.hc)
        return 0;
    return netplay_hc_confirm_through(g_b.hc, *tickp);
}

static uint64_t rb_mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Pick a load snap ≤ mismatch that the lagging peer is likely to have.
 * Live snaps only land on snap_interval ticks; never invent a phantom tick.
 *
 * Shared-frontier fast path: ticks the peer PROVABLY holds need no tip slack.
 *  - hc resolved_through: both peers simmed those ticks and core digests
 *    matched, so both saved the same interval snaps.
 *  - last committed span (g_agreed_span_lo, g_agreed_through]: both peers
 *    replayed it and resim saves a snap at EVERY replayed tick (dense).
 * Without this, mismatch one tick after a commit re-loaded a full interval +
 * slack deep (798 → 768) and each menu tap replayed ~30 ticks twice — the
 * main-menu "resim back to title" hang. Peer eviction is still covered by the
 * follower NACK path (abort, not hang).
 *
 * Fallback (no proven frontier below mismatch): interval ticks with one-
 * interval tip slack — ONLY when there is no shared frontier yet. Once
 * hash_confirm / a commit exists, never load an unconfirmed tip (those snaps
 * can fork dig_cpu while PC-cleared FRAME_COMMITs still matched). */
static int choose_load_tick(uint32_t mismatch, uint32_t *out_load)
{
    uint32_t t;
    uint32_t iv;
    uint32_t newest;
    uint32_t cap;
    uint32_t shared = 0;
    int have_shared = 0;
    if (!out_load || !g_snaps || netplay_snap_ring_count(g_snaps) == 0)
        return 0;
    iv = snap_interval();
    if (iv < 1u)
        iv = 1u;
    newest = netplay_snap_ring_newest_tick(g_snaps);

    if (g_b.hc)
        (void)netplay_hc_heal_stale_gap(g_b.hc);

    if (g_agreed_valid) {
        shared = g_agreed_through;
        have_shared = 1;
    }
    if (g_b.hc) {
        uint32_t rt = netplay_hc_resolved_through(g_b.hc);
        /* Only raise the walk ceiling on confirmed ticks — resolved_through
         * alone can sit on TipHold Live tip snaps the peer never agreed. */
        if (netplay_hc_confirm_through(g_b.hc, rt) && rt >= shared) {
            shared = rt;
            have_shared = 1;
        }
    }
    /* Snap at T = state after tick T; replaying the mismatch needs load < mismatch. */
    if (have_shared && mismatch > 0u) {
        uint32_t hi = shared;
        uint32_t lo;
        uint32_t oldest = netplay_snap_ring_oldest_tick(g_snaps);
        if (hi > mismatch - 1u)
            hi = mismatch - 1u;
        /* Bound the walk to one ring depth of ticks. */
        lo = (hi > 255u) ? hi - 255u : 0u;
        if (oldest > lo)
            lo = oldest;
        for (t = hi;; --t) {
            int mutual = 0;
            /* TipHold Live saves tip snaps past the POST tip; those are
             * local-only until hash_confirm. Never treat bare interval
             * alignment above agreed_through as peer-safe (follower NACK
             * past frontier=tip-hold commit). */
            if (g_b.hc && netplay_hc_confirm_through(g_b.hc, t))
                mutual = 1;
            else if (g_agreed_valid && t <= g_agreed_through &&
                     (t > g_agreed_span_lo || (iv <= 1u) || ((t % iv) == 0u)))
                mutual = 1;
            else if (!g_agreed_valid && ((iv <= 1u) || ((t % iv) == 0u)))
                mutual = 1;
            else if (g_fmv_dense_through > 0u && t <= g_fmv_dense_through &&
                     t >= g_fmv_media_end_sim)
                mutual = 1;
            if (mutual && netplay_snap_ring_has(g_snaps, t)) {
                *out_load = t;
                return 1;
            }
            if (t <= lo)
                break;
        }
        /* Shared frontier exists but every confirmed snap aged out of the
         * tip-dense ring — refuse rather than load an unconfirmed tip. */
        return 0;
    }
    cap = mismatch;
    if (newest >= iv && cap > newest - iv)
        cap = newest - iv;
    if (iv > 1u)
        cap -= (cap % iv);

    for (;;) {
        if (netplay_snap_ring_has(g_snaps, cap)) {
            *out_load = cap;
            return 1;
        }
        if (cap == 0u)
            break;
        if (iv > 1u)
            cap = (cap < iv) ? 0u : (cap - iv);
        else
            cap--;
    }
    /* Last resort (boot / no frontier yet): any snap ≤ mismatch. */
    if (netplay_snap_ring_has(g_snaps, mismatch)) {
        *out_load = mismatch;
        return 1;
    }
    for (t = mismatch; t > 0; --t) {
        if (netplay_snap_ring_has(g_snaps, t - 1)) {
            *out_load = t - 1;
            return 1;
        }
    }
    if (netplay_snap_ring_has(g_snaps, 0)) {
        *out_load = 0;
        return 1;
    }
    return 0;
}

/* MotK convention: SYNC with initiator=0 = follower cannot open episode
 * (missing load snap). Initiator aborts SealInputs instead of hanging. */
static void send_follow_nack(uint32_t epoch, uint32_t mismatch, uint32_t load,
                             uint32_t target, int slot, int log_line)
{
    RNetSession *s = sess();
    if (!s)
        return;
    (void)rnet_session_send_rb_sync(s, epoch, mismatch, load, target,
                                    (rnet_u8)(slot < 0 ? 0 : slot), 0u);
    if (log_line) {
        fprintf(stderr, "psxrecomp: rb follow NACK epoch=%u load=%u (snap missing)\n",
                (unsigned)epoch, (unsigned)load);
        fflush(stderr);
    }
}

static void export_local_seals(void)
{
    RNetSession *s = sess();
    RNetRbFrame rows[24];
    uint32_t count = 0;
    uint32_t begin = 0;
    uint32_t valid = 0;
    uint32_t total = 0;
    int slot;
    int i;
    if (!g_rb || !s)
        return;
    /* Only this host's authority seat — exporting empty peer slots confused
     * receivers and is unnecessary (peer seals their own local_slot). */
    slot = g_b.local_slot ? *g_b.local_slot : 0;
    begin = 0;
    while (rnet_rb_export_seal_rows_chunk(g_rb, slot, begin, 24u, rows, &count) &&
           count > 0) {
        for (i = 0; i < (int)count; ++i) {
            total++;
            if (rows[i].is_valid)
                valid++;
        }
        /* Wire "mismatch" field carries seal_base (load_tick), not correction
         * mismatch — peers index the sealed table from that base. */
        (void)rnet_session_send_rb_seal_rows(
            s, rnet_rb_get_epoch_id(g_rb), rnet_rb_get_seal_base_tick(g_rb),
            rnet_rb_get_target_tick(g_rb), (rnet_u8)slot, begin, rows, (rnet_u16)count);
        begin += count;
        if (count < 24u)
            break;
    }
    if (!g_seal_export_logged && total > 0u) {
        fprintf(stderr,
                "psxrecomp: rb seal export slot=%d rows=%u valid=%u span=%u\n",
                slot, (unsigned)total, (unsigned)valid,
                (unsigned)rnet_rb_get_seal_span(g_rb));
        fflush(stderr);
        g_seal_export_logged = 1;
    }
}

static void publish_sealed_sio(uint32_t tick)
{
    int slot;
    int n = g_b.slot_count ? *g_b.slot_count : 0;
    if (!g_rb || !g_b.apply_frame_slot)
        return;
    /* Hist first (covers any seat/tick the seal table missed), then sealed
     * authority overlays — never the reverse (hist would clobber seals). */
    if (g_b.publish_sio)
        g_b.publish_sio(tick);
    for (slot = 0; slot < n; ++slot) {
        RNetRbFrame row;
        if (!rnet_rb_get_sealed_frame(g_rb, slot, tick, &row) || !row.is_valid)
            continue;
        g_b.apply_frame_slot(slot, tick, row.buttons, row.stick_x, row.stick_y,
                             row.analog);
    }
}

static void maybe_send_baseline(void);
static void maybe_enter_replay(void);
static void accept_peer_baseline(uint32_t epoch, uint32_t load, uint32_t dig_m,
                                 uint32_t dig_a, uint32_t dig_b, uint32_t dig_c);
static void apply_stashed_baseline(void);

static void enter_awaiting_baseline(void)
{
    uint32_t load = rnet_rb_get_load_tick(g_rb);
    rnet_rb_set_phase(g_rb, nRNetRbPhaseAwaitingBaseline);
    g_local_baseline_sent = 0;
    g_local_baseline_digest = 0;
    g_local_baseline_ready_sent = 0;
    /* Keep peer baseline / ready if packets arrived during Seal. */
    g_episode_snap_applied = 0;
    g_pending_load_tick = load;
    g_pending_load_valid = 1;
    g_pending_resume_valid = 0;
    g_baseline_rexmit_logged = 0;
    g_wait_peer_bl_logged = 0;
    fprintf(stderr, "psxrecomp: rb episode load_tick=%u (snap pending)\n", (unsigned)load);
    fflush(stderr);
    apply_stashed_baseline();
}

static void accept_peer_baseline(uint32_t epoch, uint32_t load, uint32_t dig_m,
                                 uint32_t dig_a, uint32_t dig_b, uint32_t dig_c)
{
    (void)epoch;
    (void)load;
    g_peer_baseline_ok = 1;
    g_peer_baseline_digest = dig_m;
    g_peer_baseline_av = dig_b;
    g_peer_baseline_aux = dig_c;
    if (dig_a & RB_BL_FLAG_READY) {
        if (!g_peer_baseline_ready) {
            fprintf(stderr,
                    "psxrecomp: rb peer baseline ready-ACK received load=%u\n",
                    (unsigned)load);
            fflush(stderr);
        }
        g_peer_baseline_ready = 1;
    }
    fprintf(stderr,
            "psxrecomp: rb peer baseline epoch=%u load=%u core=%08x av=%08x ext=%08x "
            "ready=%u (local_applied=%d phase=%d)\n",
            (unsigned)epoch, (unsigned)load, (unsigned)dig_m, (unsigned)dig_b,
            (unsigned)dig_c, (unsigned)(dig_a & RB_BL_FLAG_READY),
            g_episode_snap_applied, (int)rnet_rb_get_phase(g_rb));
    fflush(stderr);
    maybe_send_baseline();
    maybe_enter_replay();
}

static void apply_stashed_baseline(void)
{
    if (!g_stash_bl_valid || !g_rb || !rnet_rb_is_active(g_rb))
        return;
    if (g_stash_bl_epoch != rnet_rb_get_epoch_id(g_rb))
        return;
    g_stash_bl_valid = 0;
    fprintf(stderr, "psxrecomp: rb baseline unstash epoch=%u dig=%08x ready=%u\n",
            (unsigned)g_stash_bl_epoch, (unsigned)g_stash_bl_dig_m,
            (unsigned)(g_stash_bl_dig_a & RB_BL_FLAG_READY));
    fflush(stderr);
    accept_peer_baseline(g_stash_bl_epoch, g_stash_bl_load, g_stash_bl_dig_m,
                         g_stash_bl_dig_a, g_stash_bl_dig_b, g_stash_bl_dig_c);
}

static void stash_or_accept_baseline(uint32_t epoch, uint32_t load, uint32_t dig_m,
                                     uint32_t dig_a, uint32_t dig_b, uint32_t dig_c)
{
    if (g_rb && rnet_rb_is_active(g_rb) && epoch == rnet_rb_get_epoch_id(g_rb)) {
        accept_peer_baseline(epoch, load, dig_m, dig_a, dig_b, dig_c);
        return;
    }
    /* Host often applies+sends before the follower has opened the episode —
     * dropping those packets left the guest forever without peer baseline. */
    g_stash_bl_valid = 1;
    g_stash_bl_epoch = epoch;
    g_stash_bl_load = load;
    g_stash_bl_dig_m = dig_m;
    g_stash_bl_dig_a = dig_a;
    g_stash_bl_dig_b = dig_b;
    g_stash_bl_dig_c = dig_c;
    if (!g_stash_bl_logged) {
        fprintf(stderr,
                "psxrecomp: rb baseline stashed epoch=%u load=%u dig=%08x "
                "(episode not active yet)\n",
                (unsigned)epoch, (unsigned)load, (unsigned)dig_m);
        fflush(stderr);
        g_stash_bl_logged = 1;
    }
}

static void send_baseline_packet(int ready, int rexmit_log)
{
    RNetSession *s = sess();
    if (!g_rb || !s || !g_local_baseline_sent)
        return;
    (void)rnet_session_send_rb_baseline(s, rnet_rb_get_epoch_id(g_rb),
                                        rnet_rb_get_load_tick(g_rb), g_local_baseline_digest,
                                        ready ? RB_BL_FLAG_READY : 0u, g_local_baseline_av,
                                        g_local_baseline_aux);
    if (ready)
        g_local_baseline_ready_sent = 1;
    if (rexmit_log && !g_baseline_rexmit_logged) {
        fprintf(stderr, "psxrecomp: rb baseline rexmit load=%u dig=%08x ready=%d\n",
                (unsigned)rnet_rb_get_load_tick(g_rb), (unsigned)g_local_baseline_digest, ready);
        fflush(stderr);
        g_baseline_rexmit_logged = 1;
    }
}

/* TURN loses one-shots — burst copies, paced so we don't flood the relay. */
static void send_baseline_burst(int ready, int n, int rexmit_log)
{
    int i;
    uint64_t now = rb_mono_ms();
    if (n < 1)
        n = 1;
    /* First ready=1 must not be swallowed by the rate limit after the initial
     * ready=0 burst — initiator was timing out waiting for an ACK that never
     * left (follower entered Replay with peer_ready=0 and no ready wire). */
    if (ready && !g_local_baseline_ready_sent)
        g_last_baseline_burst_ms = 0;
    if (g_last_baseline_burst_ms != 0ull && now >= g_last_baseline_burst_ms &&
        (now - g_last_baseline_burst_ms) < (uint64_t)RB_BASELINE_BURST_MS)
        return;
    g_last_baseline_burst_ms = now;
    if (ready && !g_local_baseline_ready_sent) {
        fprintf(stderr,
                "psxrecomp: rb baseline ready-ACK sent load=%u (burst=%d)\n",
                (unsigned)rnet_rb_get_load_tick(g_rb), n);
        fflush(stderr);
    }
    for (i = 0; i < n; ++i)
        send_baseline_packet(ready, rexmit_log && i == 0);
}

static void maybe_send_baseline(void)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    int follower;
    if (!g_rb || !s || !c)
        return;
    if (!g_episode_snap_applied || g_pending_load_valid)
        return; /* must restore before hashing / advertising baseline */
    follower = rnet_rb_is_from_peer_notify(g_rb) ? 1 : 0;
    if (!g_local_baseline_sent) {
        NetplayCoreParts parts;
        netplay_core_digest_parts(c, &parts);
        g_local_baseline_digest = parts.core;
        g_local_baseline_av = netplay_av_digest();
        /* dig_c folds SPU+MDEC+CD+spad+DMA+SIO — matched core/av with
         * divergent bus/scratch was doomed Replay (pin zlib skew). */
        g_local_baseline_aux = netplay_baseline_ext_digest();
        g_local_baseline_sent = 1;
        g_baseline_handshake_ms = rb_mono_ms();
        g_last_baseline_burst_ms = 0; /* allow immediate burst */
        {
            int burst = rnet_rb_recommend_light_tip(g_rb) ? 1 : RB_BASELINE_BURST;
            NetplaySioParts sp;
            send_baseline_burst(0, burst, 0);
            netplay_sio_digest_parts(&sp);
            fprintf(stderr,
                    "psxrecomp: rb baseline sent load=%u core=%08x av=%08x ext=%08x cd=%08x "
                    "(burst=%d light=%u) parts cpu=%08x clk=%08x tim=%08x ram=%08x dirty=%08x "
                    "spu=%08x mdec=%08x aux=%08x spad=%08x dma=%08x sio=%08x "
                    "sioP=%08x/%08x/%08x/%08x/%08x\n",
                    (unsigned)rnet_rb_get_load_tick(g_rb),
                    (unsigned)g_local_baseline_digest,
                    (unsigned)g_local_baseline_av, (unsigned)g_local_baseline_aux,
                    (unsigned)netplay_cdrom_digest(), burst,
                    (unsigned)rnet_rb_recommend_light_tip(g_rb),
                    (unsigned)parts.cpu, (unsigned)parts.clock_irq,
                    (unsigned)parts.timers, (unsigned)parts.ram,
                    (unsigned)parts.dirty, (unsigned)netplay_spu_digest(),
                    (unsigned)netplay_mdec_digest(),
                    (unsigned)netplay_aux_digest(),
                    (unsigned)netplay_spad_digest(), (unsigned)netplay_dma_digest(),
                    (unsigned)netplay_sio_digest(),
                    (unsigned)sp.regs, (unsigned)sp.pads, (unsigned)sp.mc,
                    (unsigned)sp.pace, (unsigned)sp.meta);
        }
        fflush(stderr);
        return;
    }
    /* Keep advertising until handshake completes. Follower upgrades to ready=1
     * once it has the peer digest. */
    if (!g_peer_baseline_ok) {
        if (!g_wait_peer_bl_logged) {
            fprintf(stderr, "psxrecomp: rb waiting peer baseline (local core=%08x)\n",
                    (unsigned)g_local_baseline_digest);
            fflush(stderr);
            g_wait_peer_bl_logged = 1;
        }
        send_baseline_burst(0, RB_BASELINE_BURST, 1);
        return;
    }
    if (follower) {
        if (!g_local_baseline_ready_sent ||
            rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline ||
            rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay)
            send_baseline_burst(1, RB_BASELINE_BURST, 1);
    } else if (!g_peer_baseline_ready) {
        send_baseline_burst(0, RB_BASELINE_BURST, 1);
    }
}

static void log_resim_tick_audit(uint32_t sim, const char *tag)
{
    CPUState *c = cpu();
    int slot;
    int n = g_b.slot_count ? *g_b.slot_count : 0;
    uint32_t dig = 0u;
    NetplayCoreParts parts;
    uint32_t av = 0u;
    uint32_t cd = 0u;
    uint32_t aux = 0u;
    uint32_t spad = 0u;
    uint32_t dma = 0u;
    uint32_t sio = 0u;
    int want_parts = (tag && tag[0] == 'f'); /* "fin" — partition breakdown */
    int present_edge = want_parts; /* fin is at vblank present */
    memset(&parts, 0, sizeof(parts));
    if (c) {
        CPUState dig_cpu;
        if (present_edge) {
            log_cpu_digest_split(c, tag);
            psx_netplay_rb_cpu_for_present_digest(&dig_cpu, c);
        } else {
            dig_cpu = *c;
            if (!rb_resume_pc_ok(dig_cpu.pc)) {
                uint32_t alt = g_last_good_bb_pc;
                if (rb_resume_pc_ok(alt))
                    dig_cpu.pc = alt;
            }
        }
        if (want_parts) {
            /* fin only: full bus digests (av = full VRAM CRC — skip on arm). */
            netplay_core_digest_parts(&dig_cpu, &parts);
            av = netplay_av_digest();
            cd = netplay_cdrom_digest();
            aux = netplay_aux_digest();
            spad = netplay_spad_digest();
            dma = netplay_dma_digest();
            sio = netplay_sio_digest();
            dig = netplay_master_digest(&dig_cpu);
        } else {
            parts.core = netplay_core_digest(&dig_cpu);
            dig = parts.core; /* arm: skip master/cd/av — diag only */
        }
    }
    fprintf(stderr,
            "psxrecomp: rb audit %s sim=%u dig=%08x core=%08x av=%08x cd=%08x cyc=%llu",
            tag ? tag : "?", (unsigned)sim, (unsigned)dig, (unsigned)parts.core,
            (unsigned)av, (unsigned)cd, (unsigned long long)psx_cycle_count);
    for (slot = 0; slot < n; ++slot) {
        RNetRbFrame row;
        if (rnet_rb_get_sealed_frame(g_rb, slot, sim, &row) && row.is_valid)
            fprintf(stderr, " s%d=%04x%s", slot, (unsigned)row.buttons,
                    row.analog ? "A" : "");
        else
            fprintf(stderr, " s%d=----", slot);
    }
    if (want_parts) {
        NetplaySioParts sp;
        netplay_sio_digest_parts(&sp);
        fprintf(stderr,
                " | cpu=%08x clk=%08x tim=%08x ram=%08x dirty=%08x aux=%08x "
                "spad=%08x dma=%08x sio=%08x sioP=%08x/%08x/%08x/%08x/%08x",
                (unsigned)parts.cpu, (unsigned)parts.clock_irq,
                (unsigned)parts.timers, (unsigned)parts.ram,
                (unsigned)parts.dirty, (unsigned)aux, (unsigned)spad,
                (unsigned)dma, (unsigned)sio,
                (unsigned)sp.regs, (unsigned)sp.pads, (unsigned)sp.mc,
                (unsigned)sp.pace, (unsigned)sp.meta);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void arm_replay_tick(uint32_t sim)
{
    publish_sealed_sio(sim);
    g_needs_advance = 1;
    g_replay_progress_ms = rb_mono_ms();
    fprintf(stderr, "psxrecomp: rb arm sim=%u (target=%u)\n", (unsigned)sim,
            (unsigned)rnet_rb_get_target_tick(g_rb));
    fflush(stderr);
    log_resim_tick_audit(sim, "arm");
}

static void arm_rereplay_after_load(uint32_t reload_tick)
{
    RNetSession *s = sess();
    uint32_t target;
    uint32_t first;

    if (!g_rb || !s)
        return;
    target = rnet_rb_get_target_tick(g_rb);
    first = reload_tick + 1u;
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, reload_tick);
    g_episode_snap_applied = 1;
    g_pending_load_valid = 0;
    if (first > target) {
        rnet_session_set_sim_tick(s, reload_tick);
        g_needs_advance = 0;
        enter_verify_at_tip(reload_tick);
        return;
    }
    rnet_session_set_sim_tick(s, first);
    arm_replay_tick(first);
}

/* Enter Verify/POST at the restored tip (empty resim span, or finish_frame
 * reached target). */
static void enter_verify_at_tip(uint32_t done)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    if (!g_rb || !s)
        return;
    if (c) {
        CPUState dig_cpu;
        log_cpu_digest_split(c, "post");
        psx_netplay_rb_cpu_for_present_digest(&dig_cpu, c);
        {
            uint32_t core = netplay_core_digest(&dig_cpu);
            uint32_t aux = netplay_aux_digest();
            uint32_t fold = 0xFFFFFFFFu;
            fold = crc32_update(fold, (const uint8_t *)&core, sizeof(core));
            fold = crc32_update(fold, (const uint8_t *)&aux, sizeof(aux));
            g_post_digest = fold ^ 0xFFFFFFFFu;
            fprintf(stderr,
                    "psxrecomp: rb post parts core=%08x aux=%08x (wire dig_m=%08x)\n",
                    (unsigned)core, (unsigned)aux, (unsigned)g_post_digest);
        }
    } else {
        g_post_digest = 0u;
    }
    g_post_av = netplay_av_digest();
    rnet_rb_set_phase(g_rb, nRNetRbPhaseVerify);
    g_verify_wait_ms = rb_mono_ms();
    (void)rnet_session_send_rb_post(s, rnet_rb_get_epoch_id(g_rb), done, g_post_digest,
                                    g_post_av, 1u);
    g_local_post_sent = 1;
    g_post_rexmit_logged = 0;
    fprintf(stderr,
            "psxrecomp: rb post sent dig_m=%08x av=%08x cd=%08x (peer_ok=%d)\n",
            (unsigned)g_post_digest, (unsigned)g_post_av,
            (unsigned)netplay_cdrom_digest(), g_peer_post_ok);
    fflush(stderr);
    if (g_peer_post_ok) {
        if (g_peer_post_digest == g_post_digest && g_peer_post_av == g_post_av)
            enter_tip_hold(rnet_rb_get_target_tick(g_rb));
        else {
            char why[128];
            if (g_peer_post_digest != g_post_digest)
                snprintf(why, sizeof(why),
                         "post core/aux diverge local=%08x peer=%08x",
                         (unsigned)g_post_digest, (unsigned)g_peer_post_digest);
            else
                snprintf(why, sizeof(why), "post av diverge local=%08x peer=%08x",
                         (unsigned)g_post_av, (unsigned)g_peer_post_av);
            rnet_rb_on_post_diverge(g_rb);
            abort_episode_realign(why);
        }
    }
}

static void maybe_enter_replay(void)
{
    RNetSession *s = sess();
    uint32_t load;
    int follower;
    if (!g_rb || !s)
        return;
    if (!g_episode_snap_applied || !g_local_baseline_sent || !g_peer_baseline_ok)
        return;
    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay)
        return; /* already armed */
    /* Never leave SealInputs with incomplete peer pads — hist invent would fork. */
    if (!rnet_rb_all_peer_seal_rows_complete(g_rb))
        return;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseAwaitingBaseline)
        return;
    if (g_peer_baseline_digest != g_local_baseline_digest) {
        char why[96];
        snprintf(why, sizeof(why), "baseline core mismatch local=%08x peer=%08x",
                 (unsigned)g_local_baseline_digest, (unsigned)g_peer_baseline_digest);
        abort_episode_realign(why);
        return;
    }
    if (g_peer_baseline_av != g_local_baseline_av) {
        char why[112];
        snprintf(why, sizeof(why), "baseline av mismatch local=%08x peer=%08x",
                 (unsigned)g_local_baseline_av, (unsigned)g_peer_baseline_av);
        abort_episode_realign(why);
        return;
    }
    if (g_peer_baseline_aux != g_local_baseline_aux) {
        char why[256];
        snprintf(why, sizeof(why),
                 "baseline ext mismatch local=%08x peer=%08x "
                 "(aux=%08x cd=%08x spad=%08x dma=%08x sio=%08x)",
                 (unsigned)g_local_baseline_aux, (unsigned)g_peer_baseline_aux,
                 (unsigned)netplay_aux_digest(), (unsigned)netplay_cdrom_digest(),
                 (unsigned)netplay_spad_digest(), (unsigned)netplay_dma_digest(),
                 (unsigned)netplay_sio_digest());
        abort_episode_realign(why);
        return;
    }
    g_episode_baseline_matched = 1;
    g_episode_load_tick = rnet_rb_get_load_tick(g_rb);
    follower = rnet_rb_is_from_peer_notify(g_rb) ? 1 : 0;
    /* Light tip: digests already match and load is tip-aligned — skip the
     * ready-ACK RTT (both peers enter Replay on digest match). Full episodes
     * keep symmetric ready (follower ACK → initiator GO). */
    if (!rnet_rb_recommend_light_tip(g_rb)) {
        if (follower) {
            send_baseline_burst(1, RB_BASELINE_BURST, 0);
            if (!g_peer_baseline_ready) {
                uint64_t now = rb_mono_ms();
                uint64_t t0 = g_baseline_handshake_ms ? g_baseline_handshake_ms : now;
                if (now >= t0 && (now - t0) >= (uint64_t)RB_READY_TIMEOUT_MS) {
                    if (!g_ready_timeout_logged) {
                        fprintf(stderr,
                                "psxrecomp: rb ready timeout — abort (no initiator GO)\n");
                        fflush(stderr);
                        g_ready_timeout_logged = 1;
                    }
                    abort_episode_realign("ready timeout (initiator never sent GO)");
                }
                return;
            }
        } else if (!g_peer_baseline_ready) {
            uint64_t now = rb_mono_ms();
            uint64_t t0 = g_baseline_handshake_ms ? g_baseline_handshake_ms : now;
            send_baseline_burst(0, RB_BASELINE_BURST, 1);
            if (now >= t0 && (now - t0) >= (uint64_t)RB_READY_TIMEOUT_MS) {
                if (!g_ready_timeout_logged) {
                    fprintf(stderr,
                            "psxrecomp: rb ready timeout — abort (no peer ready-ACK)\n");
                    fflush(stderr);
                    g_ready_timeout_logged = 1;
                }
                abort_episode_realign("ready timeout (peer never ACK'd baseline)");
            }
            return;
        } else {
            /* Initiator saw follower ACK — emit GO so follower can enter too. */
            send_baseline_burst(1, RB_BASELINE_BURST, 0);
        }
    } else {
        g_peer_baseline_ready = 1; /* treat as ready — digests already matched */
        fprintf(stderr, "psxrecomp: rb light-tip skip ready-ACK load=%u target=%u\n",
                (unsigned)rnet_rb_get_load_tick(g_rb),
                (unsigned)rnet_rb_get_target_tick(g_rb));
        fflush(stderr);
    }
    load = rnet_rb_get_load_tick(g_rb);
    /* Snap at load_tick is post-present state (already finished that tip).
     * Re-arming load + flush_resume deferred-present used to phantom
     * finish_frame before the latched VBlank IRQ ran — MotK menu wait
     * (0x8006CD54↔0x8006CDA0) then presented on opposite ping-pong edges
     * (v0=slt 1 vs v0=countdown) with a ~5-cycle skew. Skip re-finishing
     * load; first resim quantum is load+1 after the latched IRQ + one
     * real VBlank period. */
    if (g_b.hc)
        netplay_hc_prime_after(g_b.hc, load);
    rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
    g_post_rexmit_logged = 0;
    {
        uint32_t target = rnet_rb_get_target_tick(g_rb);
        uint32_t first = load + 1u;
        if (first > target) {
            /* Seal span empty — restored tip is already the target. */
            rnet_session_set_sim_tick(s, load);
            g_needs_advance = 0;
            fprintf(stderr,
                    "psxrecomp: rb replay empty span load=target=%u "
                    "(resume_pending=%d peer_ready=%d follower=%d)\n",
                    (unsigned)load, g_pending_resume_valid, g_peer_baseline_ready,
                    follower);
            fflush(stderr);
            enter_verify_at_tip(load);
        } else {
            rnet_session_set_sim_tick(s, first);
            /* Arm before flush_resume: after longjmp the guest runs to the
             * next vblank, and finish_frame must see needs_advance. */
            arm_replay_tick(first);
            fprintf(stderr,
                    "psxrecomp: rb replay %u..%u (first_arm=%u resume_pending=%d "
                    "peer_ready=%d follower=%d hc_primed_after=%u)\n",
                    (unsigned)load, (unsigned)target, (unsigned)first,
                    g_pending_resume_valid, g_peer_baseline_ready, follower,
                    (unsigned)load);
            fflush(stderr);
        }
    }
}

static void clear_post_handshake(void)
{
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
    g_peer_post_digest = 0;
    g_peer_post_av = 0;
    g_peer_post_match = 0;
    g_post_rexmit_logged = 0;
    g_verify_wait_ms = 0;
}

static void enter_tip_hold(uint32_t target)
{
    RNetSession *s = sess();
    uint32_t runway;
    uint32_t sim;
    uint32_t base;
    if (!g_rb || !rnet_rb_enter_tip_hold(g_rb))
        return;
    g_agreed_through = target;
    g_agreed_span_lo = g_episode_load_tick;
    g_agreed_valid = 1;
    if (s)
        (void)rnet_session_send_rb_resolved(s, target);
    runway = rnet_rb_get_tip_runway(g_rb);
    sim = s ? rnet_session_sim_tick(s) : target;
    /* Quiet window from the later of sealed tip and live tip — Live may already
     * sit past target when tip-extend rereplay returns to TipHold. */
    base = (sim > target) ? sim : target;
    g_tip_hold_until = base + runway;
    g_needs_advance = 0;
    clear_post_handshake();
    fprintf(stderr, "psxrecomp: rb tip-hold through=%u until=%u\n", (unsigned)target,
            (unsigned)g_tip_hold_until);
    fflush(stderr);
}

static void finalize_tip_hold(void)
{
    uint32_t target;
    if (!g_rb || !rnet_rb_is_tip_holding(g_rb))
        return;
    target = rnet_rb_get_target_tick(g_rb);
    g_episode_count++;
    fprintf(stderr, "psxrecomp: rb episode commit through=%u (tip-hold)\n", (unsigned)target);
    fflush(stderr);
    rnet_rb_session_reset(g_rb);
    g_tip_hold_until = 0;
    g_tip_extend_rereplay = 0;
    g_needs_advance = 0;
    g_episode_snap_applied = 0;
    g_pending_resume_valid = 0;
    g_episode_baseline_matched = 0;
    clear_episode_wire_state();
    clear_baseline_pin();
}

static void poll_tip_hold_finalize(void)
{
    RNetSession *s = sess();
    uint32_t sim;
    if (!g_rb || !rnet_rb_is_tip_holding(g_rb) || !s)
        return;
    sim = rnet_session_sim_tick(s);
    if (g_tip_hold_until > 0u && sim > g_tip_hold_until)
        finalize_tip_hold();
}

static void schedule_episode_rereplay(uint32_t mismatch_tick)
{
    uint32_t load = rnet_rb_get_load_tick(g_rb);
    uint32_t reload = load;
    uint32_t prefer = (mismatch_tick > 0u) ? mismatch_tick - 1u : 0u;

    if (prefer >= load && g_snaps && netplay_snap_ring_has(g_snaps, prefer))
        reload = prefer;
    else if (g_pin_valid && g_pin_tick >= load)
        reload = g_pin_tick;
    g_pending_load_tick = reload;
    g_pending_load_valid = 1;
    g_tip_extend_rereplay = 1;
    g_pending_resume_valid = 0;
    g_episode_snap_applied = 0;
    clear_post_handshake();
    rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
    fprintf(stderr,
            "psxrecomp: rb tip-extend rereplay load=%u mismatch=%u target=%u\n",
            (unsigned)reload, (unsigned)mismatch_tick,
            (unsigned)rnet_rb_get_target_tick(g_rb));
    fflush(stderr);
}

static void commit_episode(void)
{
    RNetSession *s = sess();
    uint32_t target;
    uint32_t sim_after;
    if (!g_rb)
        return;
    target = rnet_rb_get_target_tick(g_rb);
    rnet_rb_on_post_match(g_rb);
    sim_after = target + 1u;
    if (s) {
        rnet_session_set_sim_tick(s, sim_after);
        (void)rnet_session_send_rb_resolved(s, target);
    }
    g_episode_count++;
    g_agreed_through = target;
    g_agreed_span_lo = g_episode_load_tick;
    g_agreed_valid = 1;
    g_episode_baseline_matched = 0;
    fprintf(stderr, "psxrecomp: rb episode commit through=%u (count=%u)\n", (unsigned)target,
            (unsigned)g_episode_count);
    fflush(stderr);
    rnet_rb_session_reset(g_rb);
    g_tip_extend_rereplay = 0;
    g_needs_advance = 0;
    g_episode_snap_applied = 0;
    g_pending_resume_valid = 0;
    clear_episode_wire_state();
    g_bl_mismatch_streak = 0;
    /* No post-commit cooldown: promote-only after commit made char-select
     * D-pad feel rejected (hist updated, live sim not rolled). Abort/storm
     * cooldown still arms from live sim on failed episodes. */
}

void psx_netplay_rb_bind(const PsxNetplayRbBindings *b)
{
    if (!b) {
        memset(&g_b, 0, sizeof(g_b));
        g_bound = 0;
        return;
    }
    g_b = *b;
    g_bound = 1;
}

void psx_netplay_rb_start(void)
{
    RNetRbConfig cfg;
    RNetRollbackVTable vt;
    RNetSession *s = sess();
    int delay;

    psx_netplay_rb_shutdown();
    if (!g_bound || !s)
        return;

    g_snaps = netplay_snap_ring_create(NETPLAY_SNAP_RING_DEFAULT_DEPTH);
    if (!g_snaps) {
        fprintf(stderr, "psxrecomp: rb snap ring create FAILED — rewind disabled\n");
        return;
    }
    delay = g_b.input_delay ? *g_b.input_delay : 2;
    if (delay < 0)
        delay = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.local_slot = (uint32_t)(g_b.local_slot ? *g_b.local_slot : 0);
    cfg.delay = (uint32_t)delay;
    cfg.tip_runway = RNET_RB_TIP_RUNWAY_DEFAULT;
    {
        int slots = g_b.slot_count ? *g_b.slot_count : 2;
        if (slots < 2) slots = 2;
        if (slots > (int)RNET_RB_MAX_SLOTS) slots = (int)RNET_RB_MAX_SLOTS;
        cfg.slot_count = (uint32_t)slots;
    }

    memset(&vt, 0, sizeof(vt));
    vt.ctx = NULL;
    vt.save_state = host_save_state;
    vt.load_state = host_load_state;
    vt.advance_sim = host_advance_sim;
    vt.state_digest = host_state_digest;
    vt.hash_confirm_through = host_hash_confirm_through;
    vt.get_input_row = host_get_input_row;
    vt.stick_gates.hash_confirm_promote = host_hash_confirm_promote;
    /* stick_gates.ctx set per-decide if needed */

    g_rb = rnet_rb_create(&cfg, &vt);
    if (!g_rb) {
        fprintf(stderr, "psxrecomp: rb session create FAILED\n");
        netplay_snap_ring_destroy(g_snaps);
        g_snaps = NULL;
        return;
    }
    g_epoch = 1;
    g_episode_count = 0;
    g_pending_save_valid = 0;
    g_pending_load_valid = 0;
    g_pending_resume_valid = 0;
    g_episode_snap_applied = 0;
    g_agreed_valid = 0;
    g_agreed_through = 0;
    g_agreed_span_lo = 0;
    g_live_realign_pending = 0;
    g_episode_baseline_matched = 0;
    g_episode_load_tick = 0;
    g_tip_hold_until = 0;
    g_tip_extend_rereplay = 0;
    clear_baseline_pin();
    clear_episode_wire_state();
    fprintf(stderr, "psxrecomp: rb snap ring ready depth=%u snap_interval=%u\n",
            (unsigned)netplay_snap_ring_depth(g_snaps), (unsigned)snap_interval());
}

void psx_netplay_rb_shutdown(void)
{
    if (g_rb) {
        rnet_rb_destroy(g_rb);
        g_rb = NULL;
    }
    if (g_snaps) {
        netplay_snap_ring_destroy(g_snaps);
        g_snaps = NULL;
    }
    clear_baseline_pin();
    g_pending_save_valid = 0;
    g_pending_load_valid = 0;
    g_pending_resume_valid = 0;
    g_episode_snap_applied = 0;
    g_needs_advance = 0;
    g_live_realign_pending = 0;
    g_rewind_cooldown_until = 0;
    g_promote_sweep = 0;
    g_last_begin_mismatch = 0xffffffffu;
    g_was_in_fmv = 0;
    g_fmv_settle_until = 0;
    g_fmv_lockstep_until = 0;
    g_fmv_media_end_sim = 0;
    g_fmv_core_match_streak = 0;
    g_fmv_lockstep_released = 0;
    g_fmv_dense_through = 0;
    g_bl_mismatch_streak = 0;
    g_tip_hold_until = 0;
    g_tip_extend_rereplay = 0;
}

/* Dense tip snaps: media/lockstep, plus a short window after invent unlock
 * (first Cross/Start after FMV often lands in the next ~0.5s). */
static int rb_dense_snap_window(void)
{
    RNetSession *s = sess();
    uint32_t sim = s ? rnet_session_sim_tick(s) : 0u;
    if (rb_in_fmv_lockstep_window())
        return 1;
    if (g_fmv_media_end_sim &&
        sim < g_fmv_lockstep_until + 32u &&
        sim < g_fmv_media_end_sim + RB_FMV_LOCKSTEP_MAX + 32u)
        return 1;
    return 0;
}

void psx_netplay_rb_request_snap(uint32_t tick)
{
    uint32_t iv;
    if (!g_snaps)
        return;
    /* Never overwrite the frozen episode baseline (or its ring slot). */
    if (g_pin_valid && tick == g_pin_tick)
        return;
    if (g_rb && rnet_rb_is_active(g_rb) && tick == rnet_rb_get_load_tick(g_rb))
        return;
    /* Resim must keep a snap at every replayed tick (post-verify + next
     * correction). Live path rate-limits — full boot_state snaps dominate FPS.
     * Exception: media + post-FMV lockstep/+tip keep dense snaps so the first
     * invent-miss loads near the mismatch (954→944 was a ~10-tick hitch;
     * with tip snaps + shared frontier → load≈953). */
    if (!(g_rb && rnet_rb_is_resimulating(g_rb))) {
        if (!rb_dense_snap_window()) {
            iv = snap_interval();
            if (iv > 1u && (tick % iv) != 0u)
                return;
        } else if (tick >= g_fmv_dense_through) {
            g_fmv_dense_through = tick;
        }
    }
    g_pending_save_tick = tick;
    g_pending_save_valid = 1;
}

/*
 * Apply pending baseline/realign snap without longjmp.
 *
 * NEVER call from mid-guest pump (psx_cycles watchdog → psx_netplay_pump):
 * that mutates RAM/CPU while native CPS continues and causes DISPATCH FATAL
 * (guest rb-diag: pc=0 / ra=0x00FFFFFF after snap apply, before Replay).
 *
 * Safe callers:
 *  - try_admit while parked in the admit wait loop (deferred resume OK)
 *  - rb_poll only when flush_resume will immediately longjmp (live realign
 *    or already in Replay)
 */
static int try_apply_pending_load(CPUState *cpu_in)
{
    int live_realign;
    if (!g_snaps || !cpu_in || !g_pending_load_valid)
        return 0;
    if (!g_b.bios_checksum || !g_b.entry_pc)
        return 0;
    live_realign = g_live_realign_pending;
    {
        int loaded = 0;
        uint32_t loaded_tick = g_pending_load_tick;
        if (live_realign && g_pin_valid && g_pin_tick == loaded_tick && g_pin_data &&
            g_pin_size) {
            loaded = boot_state_load_buffer(g_pin_data, g_pin_size, *g_b.bios_checksum,
                                            *g_b.entry_pc, cpu_in);
        } else {
            loaded = netplay_snap_ring_load(g_snaps, loaded_tick, cpu_in, *g_b.bios_checksum,
                                            *g_b.entry_pc);
        }
        if (!loaded) {
            fprintf(stderr,
                    "psxrecomp: rb snap load FAILED tick=%u (pin=%d has=%d count=%u "
                    "newest=%u)\n",
                    (unsigned)loaded_tick, g_pin_valid && g_pin_tick == loaded_tick,
                    g_snaps && netplay_snap_ring_has(g_snaps, loaded_tick) ? 1 : 0,
                    (unsigned)(g_snaps ? netplay_snap_ring_count(g_snaps) : 0u),
                    (unsigned)(g_snaps ? netplay_snap_ring_newest_tick(g_snaps) : 0u));
            fflush(stderr);
            g_live_realign_pending = 0;
            if (!live_realign)
                abort_episode("snap load failed");
            return 0;
        }

        {
            uint32_t snap_pc = rb_canonicalize_resume_pc(cpu_in->pc);
            uint32_t pc;
            g_pending_load_valid = 0;
            g_live_realign_pending = 0;
            /* Prefer the snap's stored PC when already safe — host sticky /
             * IRQ hints can re-introduce MotK wait-loop edge forks. */
            if (rb_resume_pc_ok(snap_pc))
                pc = snap_pc;
            else
                pc = pick_snap_resume_pc(cpu_in, snap_pc);
            if (!pc) {
                char why[96];
                snprintf(why, sizeof(why),
                         "loaded snap tick=%u has no safe resume pc (snap=0x%08x)",
                         (unsigned)loaded_tick, (unsigned)snap_pc);
                if (live_realign) {
                    fprintf(stderr, "psxrecomp: rb realign FAILED — %s\n", why);
                    fflush(stderr);
                } else {
                    abort_episode(why);
                }
                return 0;
            }
            if (pc != cpu_in->pc) {
                fprintf(stderr,
                        "psxrecomp: rb snap pc rewrite tick=%u snap=0x%08x -> 0x%08x\n",
                        (unsigned)loaded_tick, (unsigned)cpu_in->pc, (unsigned)pc);
                fflush(stderr);
            }
            cpu_in->pc = pc;
            note_good_bb_pc(pc);
            psx_cycles_resync_after_restore(cpu_in);
            interrupts_resync_after_restore();
            /* Do NOT cdrom_accelerate on RB restore — host-timed CD IRQ catch-up
             * forks peers. Normalize SPU CD FIFO so host audio drain asymmetry
             * left outside the digest cannot steer XA overflow differently. */
            spu_cd_audio_reset();
            {
                extern void overlay_loader_clear_lazy_miss(void);
                overlay_loader_clear_lazy_miss();
            }
            psx_frontend_on_rb_snap_loaded();
            g_pending_resume_pc = pc;
            g_pending_resume_valid = 1;
            if (live_realign) {
                fprintf(stderr,
                        "psxrecomp: rb realign applied tick=%u pc=0x%08x core=%08x "
                        "cd=%08x\n",
                        (unsigned)loaded_tick, (unsigned)pc,
                        (unsigned)netplay_core_digest(cpu_in),
                        (unsigned)netplay_cdrom_digest());
                fflush(stderr);
                return 1;
            }
            if (g_tip_extend_rereplay) {
                g_episode_snap_applied = 1;
                g_tip_extend_rereplay = 0;
                fprintf(stderr,
                        "psxrecomp: rb tip-extend snap applied tick=%u pc=0x%08x "
                        "(episode pin kept load=%u)\n",
                        (unsigned)loaded_tick, (unsigned)pc,
                        (unsigned)g_episode_load_tick);
                fflush(stderr);
                return 1;
            }
            g_episode_snap_applied = 1;
            g_episode_load_tick = loaded_tick;
            pin_baseline_from_ring(loaded_tick);
            fprintf(stderr,
                    "psxrecomp: rb snap applied tick=%u pc=0x%08x (resume deferred)\n",
                    (unsigned)loaded_tick, (unsigned)pc);
            fflush(stderr);
            maybe_send_baseline();
            return 1;
        }
    }
}

void psx_netplay_rb_flush_resume(void)
{
    uint32_t pc;
    int live_realign_resume;
    if (!g_pending_resume_valid)
        return;
    /* Episode: must not longjmp until Replay (both baselines). Live realign
     * resumes outside any episode after POST/baseline abort. */
    live_realign_resume = !g_rb || !rnet_rb_is_active(g_rb);
    if (!live_realign_resume && rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;
    pc = g_pending_resume_pc;
    if (!rb_resume_pc_ok(pc)) {
        uint32_t alt = pick_snap_resume_pc(cpu(), pc);
        fprintf(stderr,
                "psxrecomp: rb flush_resume unsafe pc=0x%08x alt=0x%08x\n",
                (unsigned)pc, (unsigned)alt);
        fflush(stderr);
        if (!alt) {
            g_pending_resume_valid = 0;
            abort_episode("flush_resume: no safe resume pc");
            return;
        }
        pc = alt;
        g_pending_resume_pc = alt;
    }
    g_pending_resume_valid = 0;
    g_replay_progress_ms = rb_mono_ms();
    {
        CPUState *c = cpu();
        CDROMDebugState cds;
        memset(&cds, 0, sizeof(cds));
        cdrom_debug_snapshot(&cds);
        fprintf(stderr,
                "psxrecomp: rb flush_resume pc=0x%08x depth24=%d mdec_recent=%d "
                "media=%d cd_reading=%d read_delay=%d i_stat=%08x sr=%08x\n",
                (unsigned)pc, gpu_display_is_depth24(),
                mdec_recently_active(RB_FMV_MDEC_HYSTERESIS), rb_fmv_media_active(),
                cds.reading, cds.read_delay, (unsigned)cds.i_stat,
                c ? (unsigned)c->cop0[12] : 0u);
        fflush(stderr);
        /* flush_resume is called from admit inside sdl_vblank_present, which
         * runs under gpu_vblank_flush_present with s_flushing_present=1.
         * longjmp abandons that frame — drop guard AND any leftover present
         * queue (mid-while decrement) so the first post-resume BB edge does
         * not phantom-finish_frame. Do not re-arm for latched I_STAT.VBlank. */
        gpu_vblank_clear_deferred_present();
        /* longjmp also skips generated bb_defer cleanup — zero the nest. */
        {
            extern int g_psx_cyc_bb_defer;
            extern uint32_t g_psx_cyc_batch;
            g_psx_cyc_bb_defer = 0;
            g_psx_cyc_batch = 0;
        }
    }
    /* Unwind like disk savestate load so mid-block CPS is abandoned.
     * resume_at arms shared top-level-resume for sentinel / null-pc recover. */
    psx_scheduler_resume_at(pc);
    /* unreachable on success */
    fprintf(stderr, "psxrecomp: rb flush_resume RETURNED (resume failed)\n");
    fflush(stderr);
    abort_episode("flush_resume returned (longjmp failed)");
}

void psx_netplay_rb_poll(struct CPUState *cpu_in, uint32_t resume_pc)
{
    static int s_snap_fail_logged;
    static int s_snap_ok_logged;
    static int s_snap_pc_reject_logged;
    uint32_t pc;

    if (!g_snaps || !cpu_in)
        return;

    if (g_pending_save_valid && g_b.bios_checksum && g_b.entry_pc) {
        pc = pick_snap_resume_pc(cpu_in, resume_pc);
        if (!pc) {
            /* Keep pending — BB-edge poll usually has a real resume_pc. */
            if (!s_snap_pc_reject_logged) {
                fprintf(stderr,
                        "psxrecomp: rb snap save DEFERRED tick=%u — no dispatchable "
                        "PC (hint=0x%08x cpu=0x%08x irq=0x%08x check=0x%08x)\n",
                        (unsigned)g_pending_save_tick, (unsigned)resume_pc,
                        (unsigned)cpu_in->pc, (unsigned)psx_compiled_irq_resume_pc(),
                        (unsigned)psx_last_irq_check_pc());
                fflush(stderr);
                s_snap_pc_reject_logged = 1;
            }
        } else {
            CPUState snap = *cpu_in;
            snap.pc = pc;
            if (netplay_snap_ring_save(g_snaps, g_pending_save_tick, &snap,
                                       *g_b.bios_checksum, *g_b.entry_pc)) {
                note_good_bb_pc(pc);
                g_pending_save_valid = 0;
                s_snap_fail_logged = 0;
                s_snap_pc_reject_logged = 0;
                if (!s_snap_ok_logged) {
                    fprintf(stderr,
                            "psxrecomp: rb snap save ok tick=%u pc=0x%08x "
                            "interval=%u (count=%u)\n",
                            (unsigned)g_pending_save_tick, (unsigned)pc,
                            (unsigned)snap_interval(),
                            (unsigned)netplay_snap_ring_count(g_snaps));
                    fflush(stderr);
                    s_snap_ok_logged = 1;
                }
            } else if (!s_snap_fail_logged) {
                fprintf(stderr,
                        "psxrecomp: rb snap save FAILED tick=%u (ring stays empty "
                        "until saves succeed)\n",
                        (unsigned)g_pending_save_tick);
                fflush(stderr);
                s_snap_fail_logged = 1;
            }
        }
    }

    /* Apply only when flush_resume (called right after poll_snap / admit) can
     * longjmp immediately. Episode baseline loads wait for try_admit. */
    if (g_pending_load_valid) {
        int can_flush_now = g_live_realign_pending ||
                            (g_rb && rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay);
        if (can_flush_now)
            (void)try_apply_pending_load(cpu_in);
    }
}

int psx_netplay_rb_rewind_suppressed(void)
{
    RNetSession *s = sess();
    uint32_t sim;
    if (!s)
        return g_rewind_cooldown_until > 0u;
    sim = rnet_session_sim_tick(s);
    return sim < g_rewind_cooldown_until;
}

int psx_netplay_rb_fmv_defer_rewind(void)
{
    return rb_in_fmv_defer_rewind_window();
}

int psx_netplay_rb_fmv_media_active(void)
{
    return rb_fmv_media_active();
}

int psx_netplay_rb_lockstep_no_invent(void)
{
    /* Media + post-FMV lockstep: wait for remote wire (title Start / skip). */
    return rb_in_fmv_lockstep_window();
}

void psx_netplay_rb_poll_replay_stall(void)
{
    uint64_t now;
    if (!g_rb || !rnet_rb_is_active(g_rb))
        return;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;
    if (g_replay_progress_ms == 0ull)
        return;
    now = rb_mono_ms();
    if (now >= g_replay_progress_ms &&
        (now - g_replay_progress_ms) >= (uint64_t)RB_REPLAY_STALL_MS) {
        abort_episode("replay stall (no finish_frame after resume)");
    }
}

int psx_netplay_rb_take_promote_sweep(void)
{
    int v = g_promote_sweep;
    g_promote_sweep = 0;
    return v;
}

int psx_netplay_rb_tip_extend(uint32_t mismatch_tick, int slot)
{
    RNetSession *s = sess();
    RNetRbPhase phase;
    RNetRbPhase phase_at_entry;
    uint32_t old_target;
    uint32_t new_target;
    uint32_t sim;
    uint32_t load;
    int32_t corr_slot;
    int need_rereplay;
    if (!g_rb || !s || !rnet_rb_is_active(g_rb))
        return 0;
    phase = rnet_rb_get_phase(g_rb);
    phase_at_entry = phase;
    if (phase != nRNetRbPhaseSealInputs && phase != nRNetRbPhaseAwaitingBaseline &&
        phase != nRNetRbPhaseReplay && phase != nRNetRbPhaseVerify &&
        phase != nRNetRbPhaseTipHold)
        return 0;
    load = rnet_rb_get_load_tick(g_rb);
    if (mismatch_tick < load)
        return 0;
    corr_slot = rnet_rb_get_corrected_slot(g_rb);
    if (corr_slot >= 0 && slot >= 0 && corr_slot != slot)
        return 0; /* different seat — open a new episode after commit */
    old_target = rnet_rb_get_target_tick(g_rb);
    sim = rnet_session_sim_tick(s);
    new_target = old_target;
    if (mismatch_tick > new_target)
        new_target = mismatch_tick;
    if (sim > new_target)
        new_target = sim;
    need_rereplay = (phase_at_entry == nRNetRbPhaseTipHold ||
                     phase_at_entry == nRNetRbPhaseVerify || sim > mismatch_tick)
                        ? 1
                        : 0;
    /* Any TipHold/Verify leave invalidates the prior POST pair — stale peer
     * digests caused follower abort (local=new POST vs peer=pre-extend POST). */
    if (phase_at_entry == nRNetRbPhaseTipHold || phase_at_entry == nRNetRbPhaseVerify ||
        need_rereplay)
        clear_post_handshake();
    (void)rnet_rb_resign_slot_range(g_rb, slot, mismatch_tick, mismatch_tick);
    export_local_seals();
    if (new_target > old_target) {
        if (!rnet_rb_extend_target(g_rb, new_target))
            return 0;
        (void)rnet_rb_resign_slot_range(g_rb, slot, old_target + 1u, new_target);
        if (!rnet_rb_is_from_peer_notify(g_rb)) {
            (void)rnet_session_send_rb_sync(
                s, rnet_rb_get_epoch_id(g_rb), rnet_rb_get_mismatch_tick(g_rb),
                load, new_target, (rnet_u8)(slot < 0 ? 0 : slot), 1u);
        }
        export_local_seals();
        fprintf(stderr,
                "psxrecomp: rb tip-extend epoch=%u %u→%u mismatch=%u slot=%d "
                "(light=%u)\n",
                (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)old_target,
                (unsigned)new_target, (unsigned)mismatch_tick, slot,
                (unsigned)rnet_rb_recommend_light_tip(g_rb));
        fflush(stderr);
    } else if (need_rereplay) {
        (void)rnet_rb_extend_target(g_rb, old_target);
    }
    if (need_rereplay) {
        schedule_episode_rereplay(mismatch_tick);
        {
            uint32_t runway = rnet_rb_get_tip_runway(g_rb);
            uint32_t cap = new_target + runway;
            if (cap > g_tip_hold_until)
                g_tip_hold_until = cap;
        }
        fprintf(stderr,
                "psxrecomp: rb tip-extend rereplay epoch=%u mismatch=%u slot=%d "
                "until=%u\n",
                (unsigned)rnet_rb_get_epoch_id(g_rb), (unsigned)mismatch_tick, slot,
                (unsigned)g_tip_hold_until);
        fflush(stderr);
    }
    return 1;
}

int psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot)
{
    RNetRbCorrection corr;
    RNetSession *s = sess();
    uint32_t load = 0;
    uint32_t target;
    uint32_t sim;
    uint32_t snap_n;
    uint32_t snap_lo;
    uint32_t snap_hi;
    static uint32_t s_refuse_last_mismatch = 0xffffffffu;
    static uint32_t s_refuse_suppressed;

    if (!g_rb || !s)
        return 0;
    /* Prefer tip-extend over refusing while an episode is already open. */
    if (rnet_rb_is_active(g_rb))
        return psx_netplay_rb_tip_extend(mismatch_tick, slot);

    sim = rnet_session_sim_tick(s);

    /* Tick settle tracker. Media/lockstep episodes resume into MotK wait/VLC
     * tips and hang — admit waits for wire instead. */
    (void)rb_in_fmv_defer_rewind_window();
    if (rb_in_fmv_lockstep_window()) {
        static uint32_t s_fmv_refuse_sim;
        if (s_fmv_refuse_sim != sim) {
            fprintf(stderr,
                    "psxrecomp: rb begin REFUSED mismatch=%u — FMV lockstep "
                    "(no tip episode; admit waits for wire)\n",
                    (unsigned)mismatch_tick);
            fflush(stderr);
            s_fmv_refuse_sim = sim;
        }
        return 0;
    }

    /* Coalesce: refuse reopen after abort/storm only; reconcile promotes wire.
     * until is armed from live sim at failure (not rewound tip) so catch-up
     * cannot burn the window before the next d-pad edge. Clean commit does
     * not arm this. */
    if (sim < g_rewind_cooldown_until) {
        static uint32_t s_cd_log_sim;
        if (s_cd_log_sim != sim) {
            fprintf(stderr,
                    "psxrecomp: rb begin COOLDOWN mismatch=%u sim=%u until=%u "
                    "streak=%u\n",
                    (unsigned)mismatch_tick, (unsigned)sim,
                    (unsigned)g_rewind_cooldown_until,
                    (unsigned)g_bl_mismatch_streak);
            fflush(stderr);
            s_cd_log_sim = sim;
        }
        return 0;
    }
    snap_n = g_snaps ? netplay_snap_ring_count(g_snaps) : 0u;
    snap_lo = g_snaps ? netplay_snap_ring_oldest_tick(g_snaps) : 0u;
    snap_hi = g_snaps ? netplay_snap_ring_newest_tick(g_snaps) : 0u;
    if (!choose_load_tick(mismatch_tick, &load)) {
        /* One line per mismatch tick (+ suppressed count) — empty-ring thrash
         * previously wrote thousands of identical REFUSED lines per second. */
        if (mismatch_tick != s_refuse_last_mismatch) {
            if (s_refuse_suppressed > 0u) {
                fprintf(stderr,
                        "psxrecomp: rb begin REFUSED (+%u similar for mismatch=%u)\n",
                        (unsigned)s_refuse_suppressed,
                        (unsigned)s_refuse_last_mismatch);
                s_refuse_suppressed = 0;
            }
            fprintf(stderr,
                    "psxrecomp: rb begin REFUSED mismatch=%u slot=%d — no "
                    "confirmed snap (ring count=%u oldest=%u newest=%u "
                    "hc=%u agreed=%u)\n",
                    (unsigned)mismatch_tick, slot, (unsigned)snap_n,
                    (unsigned)snap_lo, (unsigned)snap_hi,
                    (unsigned)(g_b.hc ? netplay_hc_resolved_through(g_b.hc) : 0u),
                    (unsigned)(g_agreed_valid ? g_agreed_through : 0u));
            s_refuse_last_mismatch = mismatch_tick;
        } else {
            s_refuse_suppressed++;
        }
        return 0;
    }
    if (s_refuse_suppressed > 0u) {
        fprintf(stderr,
                "psxrecomp: rb begin REFUSED (+%u similar for mismatch=%u)\n",
                (unsigned)s_refuse_suppressed, (unsigned)s_refuse_last_mismatch);
        s_refuse_suppressed = 0;
        s_refuse_last_mismatch = 0xffffffffu;
    }

    target = rnet_rb_suggest_target(g_rb, mismatch_tick, sim);
    if (target < load)
        target = load;

    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = g_epoch++;
    corr.mismatch_tick = mismatch_tick;
    corr.load_tick = load;
    corr.target_tick = target;
    corr.slot = slot;
    corr.initiator = 1u;
    corr.from_peer_notify = 0u;
    /* Prefer MotK agreed tip when library watermark not yet set. */
    if (g_agreed_valid &&
        rnet_rb_is_light_tip_candidate(load, target, g_agreed_through))
        corr.flags = RNET_RB_CORR_LIGHT_TIP;

    g_last_begin_mismatch = mismatch_tick;
    rnet_rb_begin_episode(g_rb, &corr);
    clear_episode_wire_state();
    g_episode_snap_applied = 0;
    g_pending_resume_valid = 0;
    g_needs_advance = 0;
    g_seal_export_logged = 0;
    /* Seal from load_tick so every Replay quantum has authoritative pads. */
    rnet_rb_seal_inputs(g_rb, corr.load_tick, corr.target_tick, corr.slot);
    g_seal_wait_ms = rb_mono_ms();
    g_follow_nack_pending = 0;
    (void)rnet_session_send_rb_sync(s, corr.epoch_id, corr.mismatch_tick, corr.load_tick,
                                    corr.target_tick, (rnet_u8)(slot < 0 ? 0 : slot), 1u);
    export_local_seals();
    fprintf(stderr,
            "psxrecomp: rb begin epoch=%u mismatch=%u load=%u target=%u slot=%d "
            "light=%u (snaps=%u %u..%u local_slot=%d)\n",
            (unsigned)corr.epoch_id, (unsigned)mismatch_tick, (unsigned)load, (unsigned)target,
            slot, (unsigned)rnet_rb_recommend_light_tip(g_rb), (unsigned)snap_n,
            (unsigned)snap_lo, (unsigned)snap_hi,
            g_b.local_slot ? *g_b.local_slot : -1);
    fflush(stderr);

    if (rnet_rb_all_peer_seal_rows_complete(g_rb))
        enter_awaiting_baseline();
    else
        apply_stashed_baseline();
    return 1;
}

static void begin_follower(uint32_t epoch, uint32_t mismatch, uint32_t load, uint32_t target,
                           int slot)
{
    RNetRbCorrection corr;
    uint32_t snap_n;
    /* Tip-extend SYNC: same epoch, higher target — absorb, don't re-begin.
     * Must mirror initiator tip_extend: clear POST + rereplay from prior tip
     * when TipHold/Verify (follower used to arm tip-only and abort on stale
     * pre-extend peer POST). */
    if (g_rb && rnet_rb_is_active(g_rb) && epoch == rnet_rb_get_epoch_id(g_rb) &&
        load == rnet_rb_get_load_tick(g_rb) && target > rnet_rb_get_target_tick(g_rb)) {
        uint32_t old_t = rnet_rb_get_target_tick(g_rb);
        RNetRbPhase phase_at_entry = rnet_rb_get_phase(g_rb);
        int need_rereplay = (phase_at_entry == nRNetRbPhaseTipHold ||
                             phase_at_entry == nRNetRbPhaseVerify)
                                ? 1
                                : 0;
        clear_post_handshake();
        if (rnet_rb_extend_target(g_rb, target)) {
            (void)rnet_rb_resign_slot_range(g_rb, slot, old_t + 1u, target);
            (void)rnet_rb_resign_slot_range(g_rb, slot, mismatch, mismatch);
            export_local_seals();
            fprintf(stderr,
                    "psxrecomp: rb tip-extend FOLLOW epoch=%u %u→%u slot=%d "
                    "rereplay=%d\n",
                    (unsigned)epoch, (unsigned)old_t, (unsigned)target, slot,
                    need_rereplay);
            fflush(stderr);
            if (need_rereplay) {
                /* Reload at prior tip (old_t); mismatch field is episode-original. */
                schedule_episode_rereplay(old_t + 1u);
                {
                    uint32_t runway = rnet_rb_get_tip_runway(g_rb);
                    uint32_t cap = target + runway;
                    if (cap > g_tip_hold_until)
                        g_tip_hold_until = cap;
                }
            }
        }
        return;
    }
    if (!g_rb || rnet_rb_is_active(g_rb))
        return;
    (void)rb_in_fmv_defer_rewind_window();
    /* Same policy as begin: do not follow into media/lockstep tip episodes. */
    if (rb_in_fmv_lockstep_window()) {
        fprintf(stderr,
                "psxrecomp: rb follow REFUSED epoch=%u load=%u — FMV lockstep\n",
                (unsigned)epoch, (unsigned)load);
        fflush(stderr);
        g_follow_nack_pending = 1;
        g_follow_nack_epoch = epoch;
        g_follow_nack_mismatch = mismatch;
        g_follow_nack_load = load;
        g_follow_nack_target = target;
        g_follow_nack_slot = slot;
        g_follow_nack_sends = 0;
        send_follow_nack(epoch, mismatch, load, target, slot, 1);
        g_follow_nack_sends = 1;
        return;
    }
    snap_n = g_snaps ? netplay_snap_ring_count(g_snaps) : 0u;
    if (!g_snaps || !netplay_snap_ring_has(g_snaps, load)) {
        fprintf(stderr,
                "psxrecomp: rb follow REFUSED epoch=%u load=%u — snap missing "
                "(ring count=%u newest=%u)\n",
                (unsigned)epoch, (unsigned)load, (unsigned)snap_n,
                (unsigned)(g_snaps ? netplay_snap_ring_newest_tick(g_snaps) : 0u));
        fflush(stderr);
        /* Tell initiator to abort SealInputs; rexmit on TURN. */
        g_follow_nack_pending = 1;
        g_follow_nack_epoch = epoch;
        g_follow_nack_mismatch = mismatch;
        g_follow_nack_load = load;
        g_follow_nack_target = target;
        g_follow_nack_slot = slot;
        g_follow_nack_sends = 0;
        send_follow_nack(epoch, mismatch, load, target, slot, 1);
        g_follow_nack_sends = 1;
        return;
    }
    /* NACK loads past our shared frontier — initiator tip-slack beyond
     * hash_confirm is a doomed baseline (cpu-only tip snap fork). */
    if (g_b.hc)
        (void)netplay_hc_heal_stale_gap(g_b.hc);
    {
        uint32_t frontier = 0u;
        int have_f = 0;
        if (g_agreed_valid) {
            frontier = g_agreed_through;
            have_f = 1;
        }
        if (g_b.hc) {
            uint32_t rt = netplay_hc_resolved_through(g_b.hc);
            if (netplay_hc_confirm_through(g_b.hc, rt) && rt >= frontier) {
                frontier = rt;
                have_f = 1;
            }
        }
        if (have_f && load > frontier &&
            !(g_b.hc && netplay_hc_confirm_through(g_b.hc, load))) {
            fprintf(stderr,
                    "psxrecomp: rb follow REFUSED epoch=%u load=%u — past "
                    "frontier=%u (unconfirmed tip)\n",
                    (unsigned)epoch, (unsigned)load, (unsigned)frontier);
            fflush(stderr);
            g_follow_nack_pending = 1;
            g_follow_nack_epoch = epoch;
            g_follow_nack_mismatch = mismatch;
            g_follow_nack_load = load;
            g_follow_nack_target = target;
            g_follow_nack_slot = slot;
            g_follow_nack_sends = 0;
            send_follow_nack(epoch, mismatch, load, target, slot, 1);
            g_follow_nack_sends = 1;
            return;
        }
    }
    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = epoch;
    corr.mismatch_tick = mismatch;
    corr.load_tick = load;
    corr.target_tick = target;
    corr.slot = slot;
    corr.initiator = 0u;
    corr.from_peer_notify = 1u;
    if (g_agreed_valid &&
        rnet_rb_is_light_tip_candidate(load, target, g_agreed_through))
        corr.flags = RNET_RB_CORR_LIGHT_TIP;
    if (epoch >= g_epoch)
        g_epoch = epoch + 1u;
    rnet_rb_begin_episode(g_rb, &corr);
    clear_episode_wire_state();
    g_episode_snap_applied = 0;
    g_pending_resume_valid = 0;
    g_needs_advance = 0;
    g_seal_export_logged = 0;
    g_follow_nack_pending = 0;
    rnet_rb_seal_inputs(g_rb, load, target, slot);
    g_seal_wait_ms = rb_mono_ms();
    export_local_seals();
    fprintf(stderr,
            "psxrecomp: rb follow epoch=%u mismatch=%u load=%u target=%u "
            "(snaps=%u local_slot=%d)\n",
            (unsigned)epoch, (unsigned)mismatch, (unsigned)load, (unsigned)target,
            (unsigned)snap_n, g_b.local_slot ? *g_b.local_slot : -1);
    fflush(stderr);
    if (rnet_rb_all_peer_seal_rows_complete(g_rb))
        enter_awaiting_baseline();
    else
        apply_stashed_baseline();
}

void psx_netplay_rb_pump(void)
{
    RNetSession *s = sess();
    rnet_u32 epoch, mismatch, load, target, row_begin;
    rnet_u8 cslot, initiator, slot;
    rnet_u32 dig_m, dig_a, dig_b, dig_c, in_dig;
    rnet_u8 match;
    RNetRbFrame rows[24];
    rnet_u16 row_count;

    if (!g_rb || !s)
        return;

    while (rnet_session_take_rb_sync(s, &epoch, &mismatch, &load, &target, &cslot, &initiator)) {
        if (!initiator) {
            /* MotK: initiator=0 is follow-NACK (missing load snap), not an echo. */
            if (rnet_rb_is_active(g_rb) && epoch == rnet_rb_get_epoch_id(g_rb) &&
                !rnet_rb_is_from_peer_notify(g_rb)) {
                char why[96];
                snprintf(why, sizeof(why), "peer follow NACK load=%u", (unsigned)load);
                abort_episode(why);
            }
            continue;
        }
        begin_follower(epoch, mismatch, load, target, (int)cslot);
    }

    while (rnet_session_take_rb_seal_rows(s, &epoch, &mismatch, &target, &slot, &row_begin, rows,
                                         &row_count)) {
        if (!rnet_rb_is_active(g_rb))
            continue;
        if (epoch != rnet_rb_get_epoch_id(g_rb))
            continue;
        (void)rnet_rb_apply_peer_seal_rows(g_rb, epoch, mismatch, target, (int32_t)slot,
                                           row_begin, rows, row_count);
        if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs &&
            rnet_rb_all_peer_seal_rows_complete(g_rb))
            enter_awaiting_baseline();
    }

    while (rnet_session_take_rb_baseline(s, &epoch, &load, &dig_m, &dig_a, &dig_b, &dig_c)) {
        stash_or_accept_baseline(epoch, load, dig_m, dig_a, dig_b, dig_c);
    }

    while (rnet_session_take_rb_post(s, &epoch, &target, &dig_m, &in_dig, &match)) {
        if (!rnet_rb_is_active(g_rb) || epoch != rnet_rb_get_epoch_id(g_rb))
            continue;
        /* TipHold cleared the POST handshake — ignore late/rexmit POSTs from
         * the prior Verify so tip-extend cannot compare against a stale peer
         * digest. Replay/Seal/Baseline also must not latch peer_ok early. */
        if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseVerify)
            continue;
        g_peer_post_ok = 1;
        g_peer_post_digest = dig_m;
        g_peer_post_av = in_dig;
        g_peer_post_match = match;
        if (g_local_post_sent) {
            if (dig_m == g_post_digest && in_dig == g_post_av)
                enter_tip_hold(rnet_rb_get_target_tick(g_rb));
            else {
                char why[128];
                if (dig_m != g_post_digest)
                    snprintf(why, sizeof(why),
                             "post core/aux diverge local=%08x peer=%08x",
                             (unsigned)g_post_digest, (unsigned)dig_m);
                else
                    snprintf(why, sizeof(why), "post av diverge local=%08x peer=%08x",
                             (unsigned)g_post_av, (unsigned)in_dig);
                (void)g_peer_post_match;
                rnet_rb_on_post_diverge(g_rb);
                abort_episode_realign(why); /* lobby stays; Live rewinds to agreed tip */
            }
        }
    }

    {
        rnet_u32 resolved = 0;
        while (rnet_session_take_rb_resolved(s, &resolved))
            rnet_rb_set_peer_convergence(g_rb, resolved);
    }

    /* Retransmit follow-NACK while initiator may still be sealing. */
    if (g_follow_nack_pending && g_follow_nack_sends < RB_FOLLOW_NACK_REXMIT) {
        send_follow_nack(g_follow_nack_epoch, g_follow_nack_mismatch, g_follow_nack_load,
                         g_follow_nack_target, g_follow_nack_slot, 0);
        g_follow_nack_sends++;
        if (g_follow_nack_sends >= RB_FOLLOW_NACK_REXMIT)
            g_follow_nack_pending = 0;
    }

    /* TURN drops one-shot SEAL_ROWS / BASELINE easily — retransmit while waiting. */
    if (rnet_rb_is_active(g_rb) && rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs) {
        uint64_t now = rb_mono_ms();
        export_local_seals();
        if (g_seal_wait_ms != 0ull && now >= g_seal_wait_ms &&
            (now - g_seal_wait_ms) >= (uint64_t)RB_SEAL_TIMEOUT_MS) {
            abort_episode("seal timeout (peer missing snap / NACK lost)");
        }
    }

    if (rnet_rb_is_active(g_rb) &&
        rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline) {
        /* Wire/handshake only — snap apply is admit-wait exclusive. Pump runs
         * under the cycle watchdog mid-guest; mutating here is lethal. */
        apply_stashed_baseline();
        maybe_send_baseline();
        maybe_enter_replay();
    }

    /* Keep BASELINE on the wire through Replay until the peer is clearly in. */
    if (rnet_rb_is_active(g_rb) && rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay) {
        if (g_local_baseline_sent && !g_local_post_sent) {
            if (rnet_rb_is_from_peer_notify(g_rb))
                send_baseline_burst(1, RB_BASELINE_BURST, 1);
            else if (!g_peer_baseline_ready)
                send_baseline_burst(0, RB_BASELINE_BURST, 1);
        }
        /* Guest crash/hang after flush_resume: no finish_frame progress.
         * Also polled from mid-guest truncated pump (poll_replay_stall). */
        psx_netplay_rb_poll_replay_stall();
    }

    /* Retransmit POST while Verify waits for peer — do not race into live. */
    if (rnet_rb_is_active(g_rb) && rnet_rb_get_phase(g_rb) == nRNetRbPhaseVerify &&
        g_local_post_sent && !g_peer_post_ok) {
        uint64_t now = rb_mono_ms();
        if (!g_post_rexmit_logged) {
            fprintf(stderr, "psxrecomp: rb verify wait peer POST core=%08x\n",
                    (unsigned)g_post_digest);
            fflush(stderr);
            g_post_rexmit_logged = 1;
        }
        (void)rnet_session_send_rb_post(s, rnet_rb_get_epoch_id(g_rb),
                                        rnet_rb_get_target_tick(g_rb), g_post_digest, g_post_av,
                                        1u);
        if (g_verify_wait_ms == 0ull)
            g_verify_wait_ms = now;
        else if (now >= g_verify_wait_ms &&
                 (now - g_verify_wait_ms) >= (uint64_t)RB_VERIFY_TIMEOUT_MS) {
            abort_episode_realign("verify timeout (peer POST missing)");
        }
    }

    poll_tip_hold_finalize();
}

int psx_netplay_rb_active(void)
{
    return g_rb && rnet_rb_is_active(g_rb);
}

int psx_netplay_rb_is_resimulating(void)
{
    return g_rb && rnet_rb_is_resimulating(g_rb);
}

int psx_netplay_rb_tip_holding(void)
{
    return g_rb && rnet_rb_is_tip_holding(g_rb);
}

int psx_netplay_rb_abort_resim_core_mismatch(uint32_t tick, uint32_t local_core,
                                             uint32_t peer_core)
{
    char why[128];
    uint32_t load;
    CPUState *c;
    if (!g_rb || !rnet_rb_is_active(g_rb))
        return 0;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay &&
        rnet_rb_get_phase(g_rb) != nRNetRbPhaseVerify)
        return 0;
    /* Ignore stale live commits below the episode load (hc_prime_after should
     * already have cleared them; belt-and-suspenders). */
    load = rnet_rb_get_load_tick(g_rb);
    if (tick < load)
        return 0;
    snprintf(why, sizeof(why),
             "resim core diverge sim=%u local=%08x peer=%08x",
             (unsigned)tick, (unsigned)local_core, (unsigned)peer_core);
    fprintf(stderr, "psxrecomp: rb ABORT — %s (no false POST commit)\n", why);
    /* GPR-only forks with matched RAM: dump bus digests + cpu-split so logs
     * show whether scratchpad/DMA/SIO already diverged before dig_cpu. */
    c = cpu();
    if (c) {
        NetplayCoreParts parts;
        NetplaySioParts sp;
        CPUState dig_cpu;
        log_cpu_digest_split(c, "abort");
        psx_netplay_rb_cpu_for_present_digest(&dig_cpu, c);
        netplay_core_digest_parts(&dig_cpu, &parts);
        netplay_sio_digest_parts(&sp);
        fprintf(stderr,
                "psxrecomp: rb abort-parts sim=%u cpu=%08x clk=%08x tim=%08x "
                "ram=%08x dirty=%08x spad=%08x dma=%08x sio=%08x "
                "sioP=%08x/%08x/%08x/%08x/%08x aux=%08x cd=%08x av=%08x\n",
                (unsigned)tick, (unsigned)parts.cpu, (unsigned)parts.clock_irq,
                (unsigned)parts.timers, (unsigned)parts.ram,
                (unsigned)parts.dirty, (unsigned)netplay_spad_digest(),
                (unsigned)netplay_dma_digest(), (unsigned)netplay_sio_digest(),
                (unsigned)sp.regs, (unsigned)sp.pads, (unsigned)sp.mc,
                (unsigned)sp.pace, (unsigned)sp.meta,
                (unsigned)netplay_aux_digest(), (unsigned)netplay_cdrom_digest(),
                (unsigned)netplay_av_digest());
        fflush(stderr);
    }
    rnet_rb_on_post_diverge(g_rb);
    abort_episode_realign(why);
    return 1;
}

int psx_netplay_rb_load_pending(void)
{
    return g_pending_load_valid || g_pending_resume_valid || g_live_realign_pending;
}

int psx_netplay_rb_try_admit(void)
{
    RNetSession *s = sess();
    rnet_u32 sim;
    static int s_stall_logged;

    /* Live realign after abort: apply only while parked in admit wait, then
     * flush_resume longjmps (!episode active). Never leave this to pump. */
    if (g_live_realign_pending ||
        (g_pending_load_valid && (!g_rb || !rnet_rb_is_active(g_rb)))) {
        CPUState *c = cpu();
        if (c)
            (void)try_apply_pending_load(c);
        return 0;
    }

    if (!g_rb || !s || !rnet_rb_is_active(g_rb))
        return 0;

    /* Applied snap, waiting for Replay + flush_resume — do not run guest. */
    if (g_pending_resume_valid)
        return 0;

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs)
        return 0; /* wait for peer seal chunks */

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline) {
        CPUState *c = cpu();
        /* Guest is parked in admit wait — apply baseline snap here. */
        if (c)
            (void)try_apply_pending_load(c);
        maybe_send_baseline();
        maybe_enter_replay();
        /* Still waiting for flush_resume longjmp (pending) or peer baseline. */
        if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay && g_needs_advance &&
            !g_pending_resume_valid)
            return 1;
        return 0;
    }

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseVerify ||
        rnet_rb_get_phase(g_rb) == nRNetRbPhaseCommit) {
        return 0; /* wait for POST / commit */
    }

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseTipHold)
        return 0; /* Live runs; do not stall admit forever */

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay && g_pending_load_valid &&
        !g_episode_snap_applied) {
        CPUState *c = cpu();
        if (c && try_apply_pending_load(c))
            arm_rereplay_after_load(g_pending_load_tick);
        return g_needs_advance ? 1 : 0;
    }

    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return 0;

    /* Same turn as snap apply: wait for flush_resume (admit loop / BB-edge). */
    if (g_pending_resume_valid)
        return 0;

    if (g_needs_advance) {
        s_stall_logged = 0;
        return 1;
    }

    sim = rnet_session_sim_tick(s);
    if (sim > rnet_rb_get_target_tick(g_rb)) {
        if (!s_stall_logged) {
            fprintf(stderr,
                    "psxrecomp: rb try_admit stall sim=%u > target=%u (phase=Replay)\n",
                    (unsigned)sim, (unsigned)rnet_rb_get_target_tick(g_rb));
            fflush(stderr);
            s_stall_logged = 1;
        }
        return 0;
    }
    arm_replay_tick(sim);
    s_stall_logged = 0;
    return 1;
}

void psx_netplay_rb_finish_frame(void)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    rnet_u32 done;
    uint32_t target_before;
    if (!g_rb || !s || !g_needs_advance)
        return;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;

    /* First present after flush_resume — native chain is live again. */
    psx_scheduler_top_level_resume_clear();
    done = rnet_session_sim_tick(s);
    g_replay_progress_ms = rb_mono_ms();
    fprintf(stderr, "psxrecomp: rb finish_frame sim=%u target=%u\n", (unsigned)done,
            (unsigned)rnet_rb_get_target_tick(g_rb));
    fflush(stderr);
    log_resim_tick_audit(done, "fin");
    psx_netplay_rb_request_snap(done);
    if (c)
        psx_netplay_rb_poll(c, pick_snap_resume_pc(c, 0u));
    rnet_session_advance(s);
    g_needs_advance = 0;

    /* Host may have tip-extended (via reconcile before this call) — if the
     * tip grew past done, keep Replay instead of POST. */
    target_before = rnet_rb_get_target_tick(g_rb);
    if (done >= target_before)
        enter_verify_at_tip(done);
    else if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseReplay) {
        uint32_t next = done + 1u;
        if (next <= rnet_rb_get_target_tick(g_rb)) {
            rnet_session_set_sim_tick(s, next);
            arm_replay_tick(next);
        }
    }
}

uint32_t psx_netplay_rb_episode_count(void)
{
    return g_episode_count;
}

int psx_netplay_rb_phase(void)
{
    return g_rb ? (int)rnet_rb_get_phase(g_rb) : 0;
}

uint32_t psx_netplay_rb_snap_count(void)
{
    return g_snaps ? netplay_snap_ring_count(g_snaps) : 0u;
}

#endif /* PSX_HAS_RECOMP_NET */
