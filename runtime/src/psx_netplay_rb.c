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
void psx_netplay_rb_request_snap(uint32_t tick) { (void)tick; }
void psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot)
{
    (void)mismatch_tick;
    (void)slot;
}
void psx_netplay_rb_pump(void) {}
int psx_netplay_rb_active(void) { return 0; }
int psx_netplay_rb_is_resimulating(void) { return 0; }
int psx_netplay_rb_try_admit(void) { return 0; }
void psx_netplay_rb_finish_frame(void) {}
uint32_t psx_netplay_rb_episode_count(void) { return 0; }
int psx_netplay_rb_phase(void) { return 0; }
uint32_t psx_netplay_rb_snap_count(void) { return 0; }
#else

#include "netplay_hash_confirm.h"
#include "netplay_input_hist.h"
#include "netplay_snap_ring.h"
#include "netplay_state_digest.h"
#include "cdrom.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include "savestate.h"

#include "recomp_net/recomp_net.h"

#include <stdio.h>
#include <string.h>

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
static int g_local_baseline_sent;
static int g_peer_baseline_ok;
static uint32_t g_peer_baseline_digest;
static int g_local_post_sent;
static int g_peer_post_ok;
static uint32_t g_peer_post_digest;
static uint8_t g_peer_post_match;
static uint32_t g_post_digest;
static int g_needs_advance;

static RNetSession *sess(void)
{
    return (g_bound && g_b.session) ? *g_b.session : NULL;
}

static CPUState *cpu(void)
{
    return (g_bound && g_b.cpu) ? *g_b.cpu : NULL;
}

static int host_save_state(void *ctx, uint32_t tick)
{
    CPUState *c = cpu();
    (void)ctx;
    if (!g_snaps || !c || !g_b.bios_checksum || !g_b.entry_pc)
        return -1;
    return netplay_snap_ring_save(g_snaps, tick, c, *g_b.bios_checksum, *g_b.entry_pc) ? 0
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
    return netplay_master_digest(c);
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
    return netplay_ih_get(g_b.ih, (int)slot, tick, out) ? 1u : 0u;
}

static uint8_t host_hash_confirm_promote(void *ctx)
{
    uint32_t *tickp = (uint32_t *)ctx;
    if (!tickp || !g_b.hc)
        return 0;
    return netplay_hc_confirm_through(g_b.hc, *tickp);
}

static uint32_t choose_load_tick(uint32_t mismatch)
{
    uint32_t t;
    if (!g_snaps || netplay_snap_ring_count(g_snaps) == 0)
        return mismatch;
    if (netplay_snap_ring_has(g_snaps, mismatch))
        return mismatch;
    for (t = mismatch; t > 0; --t) {
        if (netplay_snap_ring_has(g_snaps, t - 1))
            return t - 1;
    }
    if (netplay_snap_ring_has(g_snaps, 0))
        return 0;
    return mismatch;
}

static void export_local_seals(void)
{
    RNetSession *s = sess();
    RNetRbFrame rows[24];
    uint32_t count = 0;
    uint32_t begin = 0;
    int slot;
    if (!g_rb || !s)
        return;
    for (slot = 0; slot < (g_b.slot_count ? *g_b.slot_count : 0); ++slot) {
        begin = 0;
        while (rnet_rb_export_seal_rows_chunk(g_rb, slot, begin, 24u, rows, &count) &&
               count > 0) {
            (void)rnet_session_send_rb_seal_rows(
                s, rnet_rb_get_epoch_id(g_rb), rnet_rb_get_mismatch_tick(g_rb),
                rnet_rb_get_target_tick(g_rb), (rnet_u8)slot, begin, rows, (rnet_u16)count);
            begin += count;
            if (count < 24u)
                break;
        }
    }
}

static void publish_sealed_sio(uint32_t tick)
{
    int slot;
    int n = g_b.slot_count ? *g_b.slot_count : 0;
    if (!g_rb || !g_b.apply_frame_slot)
        return;
    for (slot = 0; slot < n; ++slot) {
        RNetRbFrame row;
        if (!rnet_rb_get_sealed_frame(g_rb, slot, tick, &row) || !row.is_valid)
            continue;
        g_b.apply_frame_slot(slot, tick, row.buttons, row.stick_x, row.stick_y);
    }
}

static void enter_awaiting_baseline(void)
{
    uint32_t load = rnet_rb_get_load_tick(g_rb);
    rnet_rb_set_phase(g_rb, nRNetRbPhaseAwaitingBaseline);
    g_local_baseline_sent = 0;
    g_peer_baseline_ok = 0;
    g_pending_load_tick = load;
    g_pending_load_valid = 1;
    fprintf(stderr, "psxrecomp: rb episode load_tick=%u (snap pending)\n", (unsigned)load);
}

static void maybe_send_baseline(void)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    uint32_t dig;
    if (!g_rb || !s || !c || g_local_baseline_sent)
        return;
    if (g_pending_load_valid)
        return; /* wait until poll applies load */
    dig = netplay_master_digest(c);
    (void)rnet_session_send_rb_baseline(s, rnet_rb_get_epoch_id(g_rb),
                                        rnet_rb_get_load_tick(g_rb), dig, 0, 0, 0);
    g_local_baseline_sent = 1;
}

static void maybe_enter_replay(void)
{
    RNetSession *s = sess();
    if (!g_rb || !s)
        return;
    if (!g_local_baseline_sent || !g_peer_baseline_ok)
        return;
    rnet_session_set_sim_tick(s, rnet_rb_get_load_tick(g_rb));
    rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay);
    g_needs_advance = 0;
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
    fprintf(stderr, "psxrecomp: rb replay %u..%u\n",
            (unsigned)rnet_rb_get_load_tick(g_rb), (unsigned)rnet_rb_get_target_tick(g_rb));
}

static void commit_episode(void)
{
    RNetSession *s = sess();
    uint32_t target;
    if (!g_rb)
        return;
    target = rnet_rb_get_target_tick(g_rb);
    rnet_rb_on_post_match(g_rb);
    if (s) {
        rnet_session_set_sim_tick(s, target + 1u);
        (void)rnet_session_send_rb_resolved(s, target);
    }
    g_episode_count++;
    fprintf(stderr, "psxrecomp: rb episode commit through=%u (count=%u)\n", (unsigned)target,
            (unsigned)g_episode_count);
    rnet_rb_session_reset(g_rb);
    g_needs_advance = 0;
    g_local_baseline_sent = 0;
    g_peer_baseline_ok = 0;
    g_local_post_sent = 0;
    g_peer_post_ok = 0;
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
    delay = g_b.input_delay ? *g_b.input_delay : 2;
    if (delay < 0)
        delay = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.local_slot = (uint32_t)(g_b.local_slot ? *g_b.local_slot : 0);
    cfg.delay = (uint32_t)delay;

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
    g_epoch = 1;
    g_episode_count = 0;
    g_pending_save_valid = 0;
    g_pending_load_valid = 0;
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
    g_pending_save_valid = 0;
    g_pending_load_valid = 0;
    g_needs_advance = 0;
}

void psx_netplay_rb_request_snap(uint32_t tick)
{
    if (!g_snaps)
        return;
    g_pending_save_tick = tick;
    g_pending_save_valid = 1;
}

void psx_netplay_rb_poll(struct CPUState *cpu_in, uint32_t resume_pc)
{
    if (!g_snaps || !cpu_in)
        return;

    if (g_pending_save_valid && g_b.bios_checksum && g_b.entry_pc) {
        CPUState snap = *cpu_in;
        snap.pc = resume_pc;
        if (netplay_snap_ring_save(g_snaps, g_pending_save_tick, &snap, *g_b.bios_checksum,
                                   *g_b.entry_pc)) {
            g_pending_save_valid = 0;
        }
    }

    if (g_pending_load_valid && g_b.bios_checksum && g_b.entry_pc) {
        if (netplay_snap_ring_load(g_snaps, g_pending_load_tick, cpu_in, *g_b.bios_checksum,
                                   *g_b.entry_pc)) {
            g_pending_load_valid = 0;
            psx_cycles_resync_after_restore(cpu_in);
            interrupts_resync_after_restore();
            cdrom_accelerate_after_savestate();
            psx_frontend_on_savestate_loaded();
            maybe_send_baseline();
            /* Unwind like disk savestate load so mid-block CPS is abandoned. */
            psx_scheduler_resume_at(cpu_in->pc);
        } else {
            fprintf(stderr, "psxrecomp: rb snap load FAILED tick=%u\n",
                    (unsigned)g_pending_load_tick);
            g_pending_load_valid = 0;
            if (g_rb && rnet_rb_is_active(g_rb)) {
                rnet_rb_set_phase(g_rb, nRNetRbPhaseAbort);
                rnet_rb_session_reset(g_rb);
            }
        }
    }
}

void psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot)
{
    RNetRbCorrection corr;
    RNetSession *s = sess();
    uint32_t load;
    uint32_t target;
    uint32_t sim;

    if (!g_rb || !s || rnet_rb_is_active(g_rb))
        return;

    sim = rnet_session_sim_tick(s);
    load = choose_load_tick(mismatch_tick);
    target = sim > mismatch_tick ? sim : mismatch_tick;
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

    rnet_rb_begin_episode(g_rb, &corr);
    rnet_rb_seal_inputs(g_rb, corr.mismatch_tick, corr.target_tick, corr.slot);
    (void)rnet_session_send_rb_sync(s, corr.epoch_id, corr.mismatch_tick, corr.load_tick,
                                    corr.target_tick, (rnet_u8)(slot < 0 ? 0 : slot), 1u);
    export_local_seals();
    fprintf(stderr,
            "psxrecomp: rb begin epoch=%u mismatch=%u load=%u target=%u slot=%d\n",
            (unsigned)corr.epoch_id, (unsigned)mismatch_tick, (unsigned)load, (unsigned)target,
            slot);

    if (rnet_rb_all_peer_seal_rows_complete(g_rb))
        enter_awaiting_baseline();
}

static void begin_follower(uint32_t epoch, uint32_t mismatch, uint32_t load, uint32_t target,
                           int slot)
{
    RNetRbCorrection corr;
    if (!g_rb || rnet_rb_is_active(g_rb))
        return;
    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = epoch;
    corr.mismatch_tick = mismatch;
    corr.load_tick = load;
    corr.target_tick = target;
    corr.slot = slot;
    corr.initiator = 0u;
    corr.from_peer_notify = 1u;
    if (epoch >= g_epoch)
        g_epoch = epoch + 1u;
    rnet_rb_begin_episode(g_rb, &corr);
    rnet_rb_seal_inputs(g_rb, mismatch, target, slot);
    export_local_seals();
    fprintf(stderr, "psxrecomp: rb follow epoch=%u mismatch=%u load=%u target=%u\n",
            (unsigned)epoch, (unsigned)mismatch, (unsigned)load, (unsigned)target);
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
        if (!initiator)
            continue; /* we are initiator; ignore echo */
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
        (void)dig_a;
        (void)dig_b;
        (void)dig_c;
        if (!rnet_rb_is_active(g_rb) || epoch != rnet_rb_get_epoch_id(g_rb))
            continue;
        g_peer_baseline_ok = 1;
        g_peer_baseline_digest = dig_m;
        maybe_send_baseline();
        maybe_enter_replay();
    }

    while (rnet_session_take_rb_post(s, &epoch, &target, &dig_m, &in_dig, &match)) {
        (void)in_dig;
        if (!rnet_rb_is_active(g_rb) || epoch != rnet_rb_get_epoch_id(g_rb))
            continue;
        g_peer_post_ok = 1;
        g_peer_post_digest = dig_m;
        g_peer_post_match = match;
        if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseVerify && g_local_post_sent) {
            if (g_peer_post_match && dig_m == g_post_digest)
                commit_episode();
            else {
                fprintf(stderr, "psxrecomp: rb post diverge local=%08x peer=%08x\n",
                        (unsigned)g_post_digest, (unsigned)dig_m);
                rnet_rb_on_post_diverge(g_rb);
                rnet_rb_session_reset(g_rb);
            }
        }
    }

    {
        rnet_u32 resolved = 0;
        while (rnet_session_take_rb_resolved(s, &resolved))
            rnet_rb_set_peer_convergence(g_rb, resolved);
    }

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline) {
        maybe_send_baseline();
        maybe_enter_replay();
    }
}

int psx_netplay_rb_active(void)
{
    return g_rb && rnet_rb_is_active(g_rb);
}

int psx_netplay_rb_is_resimulating(void)
{
    return g_rb && rnet_rb_is_resimulating(g_rb);
}

int psx_netplay_rb_try_admit(void)
{
    RNetSession *s = sess();
    rnet_u32 sim;
    if (!g_rb || !s || !rnet_rb_is_active(g_rb))
        return 0;

    if (rnet_rb_get_phase(g_rb) == nRNetRbPhaseSealInputs ||
        rnet_rb_get_phase(g_rb) == nRNetRbPhaseAwaitingBaseline)
        return 0; /* wait for seals / snap load */

    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return 0;

    if (g_needs_advance)
        return 1;

    sim = rnet_session_sim_tick(s);
    if (sim > rnet_rb_get_target_tick(g_rb)) {
        /* Should have finished already. */
        return 0;
    }
    publish_sealed_sio(sim);
    g_needs_advance = 1;
    return 1;
}

void psx_netplay_rb_finish_frame(void)
{
    RNetSession *s = sess();
    CPUState *c = cpu();
    rnet_u32 done;
    if (!g_rb || !s || !g_needs_advance)
        return;
    if (rnet_rb_get_phase(g_rb) != nRNetRbPhaseReplay)
        return;

    done = rnet_session_sim_tick(s);
    psx_netplay_rb_request_snap(done);
    rnet_session_advance(s);
    g_needs_advance = 0;

    if (done >= rnet_rb_get_target_tick(g_rb)) {
        g_post_digest = c ? netplay_master_digest(c) : 0u;
        rnet_rb_set_phase(g_rb, nRNetRbPhaseVerify);
        (void)rnet_session_send_rb_post(s, rnet_rb_get_epoch_id(g_rb), done, g_post_digest, 0u,
                                        1u);
        g_local_post_sent = 1;
        if (g_peer_post_ok) {
            if (g_peer_post_match && g_peer_post_digest == g_post_digest)
                commit_episode();
            else {
                fprintf(stderr, "psxrecomp: rb post diverge local=%08x peer=%08x\n",
                        (unsigned)g_post_digest, (unsigned)g_peer_post_digest);
                rnet_rb_on_post_diverge(g_rb);
                rnet_rb_session_reset(g_rb);
            }
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
