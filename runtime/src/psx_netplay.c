#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "psx_netplay.h"

#include "memcard.h"
#include "savestate.h"
#include "sio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(PSX_HAS_RECOMP_NET)
#include "recomp_net/recomp_net.h"
#include "recomp_net/input_contract.h"
#include "netplay_hash_confirm.h"
#include "netplay_input_hist.h"
#include "netplay_state_digest.h"
#include "psx_netplay_rb.h"
#include "cpu_state.h"
#include "gpu.h"
#include "interrupts.h"
#include "mdec.h"
#include "psx_scheduler.h"
#if defined(PSX_HAS_LOBBY_CLIENT)
#include "psx_lobby_client.h"
#endif
#endif

#ifndef PSX_MAX_PLAYERS
#define PSX_MAX_PLAYERS 2
#endif

/* Session pad count mirrored for release_pads (available without recomp-net). */
static int g_np_slot_count = 2;

/* Persists across shutdown so starvation dumps still see last session topology. */
static char g_np_diag_arch[24] = "off";
static int  g_np_diag_max_players = 0;
static int  g_np_diag_player_count = 0;
static int  g_np_diag_configured = 0;

int psx_netplay_diag_snapshot(char *arch_out, size_t arch_cap,
                              int *max_players_out, int *player_count_out)
{
    if (arch_out && arch_cap)
        snprintf(arch_out, arch_cap, "%s", g_np_diag_arch);
    if (max_players_out) *max_players_out = g_np_diag_max_players;
    if (player_count_out) *player_count_out = g_np_diag_player_count;
    return g_np_diag_configured;
}

void psx_netplay_config_defaults(PsxNetplayConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->local_slot = 0;
    cfg->slot_count = 2;
    cfg->player_count = 0;
    cfg->input_player = -1;
    cfg->input_delay = 2;
    cfg->input_prediction = 4;
    cfg->force_input_relay = 0;
    cfg->force_turn = 0;
    cfg->transport = 0;
    cfg->session_id = 1;
    strncpy(cfg->bind_hostport, "0.0.0.0:7777", sizeof(cfg->bind_hostport) - 1);
    cfg->peer_hostport[0] = '\0';
}

static unsigned env_u(const char *name, unsigned def)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    return (unsigned)strtoul(v, NULL, 10);
}

void psx_netplay_apply_env(PsxNetplayConfig *cfg)
{
    const char *v;
    if (!cfg) return;
    v = getenv("PSX_NETPLAY");
    if (v && v[0] && v[0] != '0') cfg->enabled = 1;
    v = getenv("PSX_NET_SLOT");
    if (v && v[0]) cfg->local_slot = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_SLOTS");
    if (v && v[0]) cfg->slot_count = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_INPUT_PLAYER");
    if (v && v[0]) cfg->input_player = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_DELAY");
    if (v && v[0]) cfg->input_delay = (int)strtol(v, NULL, 10);
    v = getenv("PSX_NET_PREDICTION");
    if (v && v[0]) cfg->input_prediction = (int)strtol(v, NULL, 10);
    cfg->session_id = env_u("PSX_NET_SESSION_ID", cfg->session_id);
    v = getenv("PSX_NET_BIND");
    if (v && v[0]) {
        strncpy(cfg->bind_hostport, v, sizeof(cfg->bind_hostport) - 1);
        cfg->bind_hostport[sizeof(cfg->bind_hostport) - 1] = '\0';
    }
    v = getenv("PSX_NET_PEER");
    if (v && v[0]) {
        strncpy(cfg->peer_hostport, v, sizeof(cfg->peer_hostport) - 1);
        cfg->peer_hostport[sizeof(cfg->peer_hostport) - 1] = '\0';
    }
    v = getenv("PSX_NET_TRANSPORT");
    if (v && v[0]) {
        if (strcmp(v, "ice") == 0 || strcmp(v, "ICE") == 0)
            cfg->transport = 1;
        else if (strcmp(v, "lan") == 0 || strcmp(v, "LAN") == 0)
            cfg->transport = 2;
    }
    v = getenv("PSX_NET_FORCE_TURN");
    if (v && v[0] && v[0] != '0')
        cfg->force_turn = 1;
    v = getenv("PSX_NET_MODE");
    if (v && v[0]) {
        if (strcmp(v, "rollback") == 0 || strcmp(v, "rb") == 0)
            cfg->rollback = 1;
        else if (strcmp(v, "delay") == 0 || strcmp(v, "delay-sync") == 0)
            cfg->rollback = 0;
    }
}

void psx_netplay_normalize_pad(PsxNetPad *pad)
{
    const int dead = 24; /* ~SDL-ish center deadzone in 0..255 space */
    if (!pad) return;
    pad->connected = 1;
    if (pad->lx > (uint8_t)(0x80 - dead) && pad->lx < (uint8_t)(0x80 + dead)) pad->lx = 0x80;
    if (pad->ly > (uint8_t)(0x80 - dead) && pad->ly < (uint8_t)(0x80 + dead)) pad->ly = 0x80;
    if (pad->rx > (uint8_t)(0x80 - dead) && pad->rx < (uint8_t)(0x80 + dead)) pad->rx = 0x80;
    if (pad->ry > (uint8_t)(0x80 - dead) && pad->ry < (uint8_t)(0x80 + dead)) pad->ry = 0x80;
    if (!pad->analog) {
        pad->lx = pad->ly = pad->rx = pad->ry = 0x80;
    }
}

static void force_session_pads_connected(int slot_count)
{
    int i;
    if (slot_count < 2) slot_count = 2;
    if (slot_count > PSX_MAX_PLAYERS) slot_count = PSX_MAX_PLAYERS;
    if (slot_count >= 3)
        sio_set_multitap(1);
    else
        sio_set_multitap(0);
    for (i = 0; i < slot_count; ++i) {
        sio_connect_pad(i);
        /* Multitap taps are plain digital (sio clamps); lone port pad may be DS. */
        sio_set_pad_config_capable(i, sio_pad_on_multitap(i) ? 0 : 1);
    }
}

void psx_netplay_release_pads(void)
{
    int i;
    int n = g_np_slot_count;
    if (n < 2) n = 2;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    force_session_pads_connected(n);
    for (i = 0; i < n; ++i) {
        sio_set_pad_state_slot(i, 0xFFFFu);
        sio_set_pad_sticks(i, 0x80, 0x80, 0x80, 0x80);
        /* Linking placeholder: digital until tip/hist publishes the real type.
         * Forcing DualShock here broke MotK (game.toml default_mode=digital). */
        sio_request_pad_type(i, 0);
    }
}

#if !defined(PSX_HAS_RECOMP_NET)

int  psx_netplay_active(void) { return 0; }
int  psx_netplay_is_running(void) { return 0; }
const char *psx_netplay_transport_name(void) { return "none"; }
int  psx_netplay_ice_failed(void) { return 0; }
void psx_netplay_diag_tick(void) {}
int  psx_netplay_local_slot(void) { return -1; }
int  psx_netplay_input_player(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
int  psx_netplay_start(const PsxNetplayConfig *cfg)
{
    (void)cfg;
    return -1;
}
void psx_netplay_shutdown(void) {}
void psx_netplay_stage_local(const PsxNetPad *pad) { (void)pad; }
int  psx_netplay_needs_local_sample(void) { return 0; }
int  psx_netplay_live_pad_buttons(uint16_t *out)
{
    if (out)
        *out = 0xFFFFu;
    return 0;
}
int  psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    (void)tick;
    (void)local_hash;
    (void)remote_hash;
    return 0;
}
int  psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}
void psx_netplay_bind_guest_saves(void) {}
int  psx_netplay_is_host(void) { return 0; }
int  psx_netplay_request_save(int slot) { (void)slot; return 0; }
int  psx_netplay_request_load(int slot) { (void)slot; return 0; }
int  psx_netplay_in_load_barrier(void) { return 0; }
int  psx_netplay_consume_load_apply_failed(void) { return 0; }
void psx_netplay_pump(void) {}
int  psx_netplay_poll_admit(void) { return 1; }
void psx_netplay_finish_frame(void) {}
int  psx_netplay_remote_lead(void) { return 0; }
int  psx_netplay_input_delay(void) { return 2; }
void psx_netplay_timesync_on_episode_boundary(void) {}
int  psx_netplay_catchup_budget(void) { return 0; }
void psx_netplay_catchup_consume_frame(void) {}
void psx_netplay_wait_recv(int timeout_ms) { (void)timeout_ms; }
void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out)
{
    if (stall_out && stall_cap) {
        stall_out[0] = '\0';
        if (stall_cap > 1)
            strncpy(stall_out, "off", stall_cap - 1);
    }
    if (sim_tick_out) *sim_tick_out = 0;
    if (lead_out) *lead_out = 0;
}
void psx_netplay_bind_cpu(struct CPUState *cpu) { (void)cpu; }
uint32_t psx_netplay_resolved_through(void) { return 0; }
int psx_netplay_hash_confirm_through(uint32_t tick) { (void)tick; return 0; }
int psx_netplay_rollback_mode(void) { return 0; }
void psx_netplay_poll_snap(struct CPUState *cpu, uint32_t resume_pc)
{
    (void)cpu;
    (void)resume_pc;
}
int psx_netplay_is_resimulating(void) { return 0; }

#else /* PSX_HAS_RECOMP_NET */

#define NP_SANDBOX_FALLBACK "saves/netplay"
#define NP_MC_BLOB_BYTES (4u + (size_t)MEMCARD_SIZE * 2u)
/* LOAD probe size==0 + this crc = post-load ready rendezvous (not SAVE coord). */
#define NP_LOAD_READY_CRC 0x4C4F4144u /* 'LOAD' */

typedef enum {
    NP_XFER_NONE = 0,
    NP_XFER_MC_PROBE,
    NP_XFER_MC_SEND,
    NP_XFER_SAVE_COORD,
    NP_XFER_SAVE_PROBE,
    NP_XFER_SAVE_SEND,
    NP_XFER_LOAD_PROBE,
    NP_XFER_LOAD_SEND,
    NP_XFER_LOAD_APPLYING, /* load staged; admit runs until savestate_poll fires */
    NP_XFER_LOAD_READY     /* local restore done; wait peer before lockstep */
} NpXferPhase;

typedef struct {
    RNetSession *session;
    PsxNetPad    staged;
    int          staged_valid;
    /* Physical pad refreshed every stage_local call (even when latched). */
    PsxNetPad    live;
    int          live_valid;
    int          active;
    int          slot_count;
    int          local_slot;
    int          input_player; /* resolved host PlayerInput index */
    int          needs_advance;
    int          latched_for_tick; /* 1 if staged pad frozen for current sim_tick */
    uint32_t     latched_sim_tick;
    /* Guest sandbox: personal roots restored on shutdown. */
    int          guest_sandbox;
    char         personal_save_dir[512];
    char         personal_mc0[512];
    char         personal_mc1[512];
    uint32_t     bios_checksum;
    uint32_t     entry_pc;
    /* Host-owned save/memcard sync. */
    NpXferPhase  xfer;
    int          xfer_slot;
    int          mc_sync_done;
    int          mc_sync_sent;
    int          local_save_staged;
    int          local_save_acked;   /* guest: coord reply already sent */
    uint32_t     save_target_tick;   /* both peers save during this sim_tick */
    int          load_applied_local;
    int          load_ready_replied; /* READY exchanged; synced; stay LOAD_READY until admit */
    int          load_sync_done;     /* hard_resync+prime once at mutual ready */
    int          load_apply_failed;  /* sticky: staged apply rejected — soft-exit */
    /* Transport / ICE / diag (MotK online path). */
    int          use_ice;
    int          ice_has_turn;
    int          force_input_relay;
    int          is_host;
    int          input_delay;
    int          input_prediction; /* invent lead cap (P); rollback only */
    uint32_t     session_id;
    uint32_t     frames_finished;
    uint32_t     diag_session;
    unsigned     ice_stun_port;
    unsigned     ice_turn_port;
    char         ice_stun_host[128];
    char         ice_turn_host[128];
    char         ice_turn_user[192];
    char         ice_turn_pass[128];
    char         ice_bind_addr[64];
    char         bind_hostport[64];
    char         peer_hostport[64];
    char         match_mode[32];
    char         lobby_server[256];
    char         lobby_id[64];
    /* Master digest / FRAME_COMMIT watermark (rollback hash_confirm). */
    CPUState*          cpu;
    NetplayHashConfirm hc;
    /* Rollback invent / stick-replace contract (PSX_NET_MODE=rollback). */
    int                rollback;
    NetplayInputHist   ih;
    /* Pending rewind from late wire (episode hookup is step 4). */
    int                pending_rewind;
    uint32_t           pending_rewind_tick;
    int                pending_rewind_slot;
} NetplayState;

static NetplayState g_np;

/* Local partition ring aligned with FRAME_COMMIT — explain first core fork. */
#define NP_PART_RING 128u
typedef struct NpPartSlot {
    uint32_t tick;
    NetplayCoreParts parts;
    uint32_t av;
    uint32_t cd;
    uint32_t aux;
    uint8_t  valid;
} NpPartSlot;
static NpPartSlot s_part_ring[NP_PART_RING];
static int s_core_diverge_logged;
static uint32_t s_live_dig_last_tick = 0xffffffffu;

static void np_part_ring_reset(void)
{
    memset(s_part_ring, 0, sizeof(s_part_ring));
    s_core_diverge_logged = 0;
    s_live_dig_last_tick = 0xffffffffu;
}

static void np_part_ring_put(uint32_t tick, const NetplayCoreParts *parts,
                             uint32_t av, uint32_t cd, uint32_t aux)
{
    NpPartSlot *s = &s_part_ring[tick % NP_PART_RING];
    s->tick = tick;
    s->parts = *parts;
    s->av = av;
    s->cd = cd;
    s->aux = aux;
    s->valid = 1u;
}

static const NpPartSlot *np_part_ring_get(uint32_t tick)
{
    const NpPartSlot *s = &s_part_ring[tick % NP_PART_RING];
    if (!s->valid || s->tick != tick)
        return NULL;
    return s;
}

static void np_log_live_digest(uint32_t tick, const NetplayCoreParts *parts,
                               uint32_t av, uint32_t cd, uint32_t aux,
                               const char *tag)
{
    fprintf(stderr,
            "psxrecomp: rb live dig %s sim=%u core=%08x cpu=%08x clk=%08x "
            "tim=%08x ram=%08x dirty=%08x av=%08x cd=%08x aux=%08x\n",
            tag ? tag : "tick", (unsigned)tick, (unsigned)parts->core,
            (unsigned)parts->cpu, (unsigned)parts->clock_irq,
            (unsigned)parts->timers, (unsigned)parts->ram,
            (unsigned)parts->dirty, (unsigned)av, (unsigned)cd,
            (unsigned)aux);
    fflush(stderr);
}

static void np_check_core_diverge(void)
{
    uint32_t tick = 0, local_d = 0, peer_d = 0;
    const NpPartSlot *slot;
    if (!netplay_hc_peek_mismatch(&g_np.hc, &tick, &local_d, &peer_d))
        return;
    /* Replay/Verify: never allow a false POST commit after mid-resim fork. */
    if (psx_netplay_rb_abort_resim_core_mismatch(tick, local_d, peer_d))
        return;
    if (s_core_diverge_logged)
        return;
    s_core_diverge_logged = 1;
    slot = np_part_ring_get(tick);
    if (slot) {
        fprintf(stderr,
                "psxrecomp: rb FIRST CORE DIVERGE sim=%u local=%08x peer=%08x "
                "| local parts cpu=%08x clk=%08x tim=%08x ram=%08x dirty=%08x "
                "av=%08x cd=%08x aux=%08x (compare peer rb live dig at same sim)\n",
                (unsigned)tick, (unsigned)local_d, (unsigned)peer_d,
                (unsigned)slot->parts.cpu, (unsigned)slot->parts.clock_irq,
                (unsigned)slot->parts.timers, (unsigned)slot->parts.ram,
                (unsigned)slot->parts.dirty, (unsigned)slot->av,
                (unsigned)slot->cd, (unsigned)slot->aux);
    } else {
        fprintf(stderr,
                "psxrecomp: rb FIRST CORE DIVERGE sim=%u local=%08x peer=%08x "
                "(local parts aged out of ring — see prior rb live dig lines)\n",
                (unsigned)tick, (unsigned)local_d, (unsigned)peer_d);
    }
    fflush(stderr);
}

static void np_drain_peer_frame_commits(void)
{
    rnet_u32 through = 0, hash = 0;
    if (!g_np.session) return;
    while (rnet_session_take_rb_frame_commit(g_np.session, &through, &hash)) {
        /* Tip-extend rereplay: TipHold Live invent FCs may still be queued
         * after hc_prime — drop until peer's sealed resim matches. */
        if (g_np.rollback &&
            psx_netplay_rb_ignore_peer_frame_commit(through, hash))
            continue;
        netplay_hc_note_peer(&g_np.hc, through, hash);
    }
    /* FIRST CORE can stick the watermark forever once that tick ages out of
     * the 128-slot ring — heal so choose_load_tick sees a real frontier. */
    if (netplay_hc_heal_stale_gap(&g_np.hc)) {
        static uint32_t s_heal_log;
        uint32_t rt = netplay_hc_resolved_through(&g_np.hc);
        if (s_heal_log != rt) {
            fprintf(stderr,
                    "psxrecomp: rb hash_confirm heal stale gap → resolved=%u\n",
                    (unsigned)rt);
            fflush(stderr);
            s_heal_log = rt;
        }
    }
    np_check_core_diverge();
}

static void np_emit_frame_commit(uint32_t tick)
{
    NetplayCoreParts parts;
    CPUState dig_cpu;
    uint32_t av = 0u;
    uint32_t cd = 0u;
    uint32_t aux = 0u;
    int crumb;
    if (!g_np.cpu || !g_np.session) return;
    /* TipHold Live invents are not sealed-input truth — emitting them poisoned
     * tip-extend Replay (stale peer digests → false resim core diverge). */
    if (g_np.rollback && psx_netplay_rb_tip_holding() &&
        !psx_netplay_rb_is_resimulating())
        return;
    /* Live depth24 + hot MDEC: skip full-RAM FRAME_COMMIT (rewind already
     * deferred). Resim still commits every tick. MDEC-idle cutover still
     * commits so FMV1→FMV2 forks are visible (digs used to go dark for the
     * whole movie). Leaving FMV primes hash_confirm so the watermark is not
     * stuck on missing movie slots. */
    if (gpu_display_is_depth24() && mdec_recently_active(8) &&
        !(g_np.rollback && psx_netplay_rb_is_resimulating()))
        return;
    /* Present-edge: clear PC so parked-0 vs live-BB does not fork FRAME_COMMIT
     * while GPRs/RAM/clk match (was aborting good Replay on dig_cpu alone). */
    psx_netplay_rb_cpu_for_present_digest(&dig_cpu, g_np.cpu);
    netplay_core_digest_parts(&dig_cpu, &parts);
    /* av/cd/aux every 32 ticks — VRAM + SPU-RAM CRC every frame is too heavy. */
    crumb = (tick == 0u || (tick % 32u) == 0u);
    if (crumb) {
        av = netplay_av_digest();
        cd = netplay_cdrom_digest();
        aux = netplay_aux_digest();
    }
    np_part_ring_put(tick, &parts, av, cd, aux);
    netplay_hc_note_local(&g_np.hc, tick, parts.core);
    (void)rnet_session_send_rb_frame_commit(g_np.session, tick, parts.core);
    (void)netplay_hc_heal_stale_gap(&g_np.hc);
    /* Breadcrumbs so both peers' logs line up by sim tick. Tag is local-only
     * (not peer agreement — that is hash_confirm resolved_through). */
    if (crumb && s_live_dig_last_tick != tick) {
        s_live_dig_last_tick = tick;
        np_log_live_digest(tick, &parts, av, cd, aux, "local");
    }
    np_check_core_diverge();
}

static FILE *g_diag_file;
static uint32_t g_diag_file_session;
static int g_diag_summary_written;
static uint32_t g_diag_last_write_ms;
static int g_diag_mkdir_done;

static void np_sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

static uint32_t np_mono_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint32_t)((uint64_t)ts.tv_sec * 1000ull +
                          (uint64_t)ts.tv_nsec / 1000000ull);
#endif
#if defined(_WIN32)
    return (uint32_t)GetTickCount64();
#else
    return (uint32_t)((uint64_t)time(NULL) * 1000ull);
#endif
}

static void np_enter_load_ready(int slot);
static void np_commit_load_sync(void);
static void np_begin_load_apply(int slot);
static void np_starv_reset(void);
static void np_maybe_stage_target_save(void);

static int np_file_crc(const uint8_t *data, size_t size, uint32_t *crc_out)
{
    if (!data || size == 0 || !crc_out) return 0;
    *crc_out = rnet_checksum(data, size);
    return 1;
}

static int np_slot_crc(int slot, uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t crc;
    if (!savestate_read_slot(slot, &data, &size) || !data) return 0;
    if (!np_file_crc(data, size, &crc)) {
        free(data);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)size;
    if (crc_out) *crc_out = crc;
    free(data);
    return 1;
}

static int np_build_mc_blob(uint8_t *out, size_t cap, size_t *out_size)
{
    uint8_t *p;
    if (!out || cap < NP_MC_BLOB_BYTES || !out_size) return -1;
    memset(out, 0, NP_MC_BLOB_BYTES);
    p = out;
    p[0] = memcard_is_present(0) ? 1u : 0u;
    p[1] = memcard_is_present(1) ? 1u : 0u;
    if (p[0] && memcard_export_raw(0, p + 4) != 0) return -1;
    if (p[1] && memcard_export_raw(1, p + 4 + MEMCARD_SIZE) != 0) return -1;
    *out_size = NP_MC_BLOB_BYTES;
    return 0;
}

static int np_apply_mc_blob(const uint8_t *data, size_t size)
{
    if (!data || size < NP_MC_BLOB_BYTES) return -1;
    if (data[0]) {
        if (memcard_import_raw(0, data + 4) != 0) return -1;
    }
    if (data[1]) {
        if (memcard_import_raw(1, data + 4 + MEMCARD_SIZE) != 0) return -1;
    }
    return 0;
}

static int np_mc_blob_crc(uint32_t *size_out, uint32_t *crc_out)
{
    uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
    size_t sz = 0;
    uint32_t crc;
    if (!blob) return 0;
    if (np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0) {
        free(blob);
        return 0;
    }
    if (!np_file_crc(blob, sz, &crc)) {
        free(blob);
        return 0;
    }
    if (size_out) *size_out = (uint32_t)sz;
    if (crc_out) *crc_out = crc;
    free(blob);
    return 1;
}

static int np_xfer_busy(void)
{
    if (!g_np.session) return 0;
    if (g_np.xfer != NP_XFER_NONE) return 1;
    return rnet_session_state_busy(g_np.session) ||
           rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL);
}

static void np_enter_guest_sandbox(void)
{
    const char *dir = savestate_dir();
    const char *p0 = NULL;
    const char *p1 = NULL;
    uint32_t bios = 0, entry = 0;
    char sandbox[560];

    savestate_get_integrity(&bios, &entry);
    g_np.bios_checksum = bios;
    g_np.entry_pc = entry;
    if (dir && dir[0])
        strncpy(g_np.personal_save_dir, dir, sizeof(g_np.personal_save_dir) - 1);
    (void)memcard_debug_info(0, &p0, NULL, NULL, NULL);
    (void)memcard_debug_info(1, &p1, NULL, NULL, NULL);
    if (p0) strncpy(g_np.personal_mc0, p0, sizeof(g_np.personal_mc0) - 1);
    if (p1) strncpy(g_np.personal_mc1, p1, sizeof(g_np.personal_mc1) - 1);

    /* Prefer <memcard_dir>/netplay (absolute, next to the binary) so CWD does
     * not matter. Relative "saves/netplay" only as a last-resort fallback. */
    if (g_np.personal_save_dir[0]) {
        size_t n = strlen(g_np.personal_save_dir);
        while (n > 0 && (g_np.personal_save_dir[n - 1] == '/' ||
                         g_np.personal_save_dir[n - 1] == '\\')) {
            g_np.personal_save_dir[--n] = '\0';
        }
        snprintf(sandbox, sizeof(sandbox), "%s/netplay", g_np.personal_save_dir);
    } else {
        snprintf(sandbox, sizeof(sandbox), "%s", NP_SANDBOX_FALLBACK);
    }

    savestate_configure(sandbox, bios, entry);
    (void)memcard_rebind_dir(sandbox);
    g_np.guest_sandbox = 1;
    printf("psxrecomp: netplay guest sandbox -> %s\n", sandbox);
    fflush(stdout);
}

static void np_leave_guest_sandbox(void)
{
    if (!g_np.guest_sandbox) return;
    memcard_flush_all();
    (void)memcard_rebind_paths(
        g_np.personal_mc0[0] ? g_np.personal_mc0 : NULL,
        g_np.personal_mc1[0] ? g_np.personal_mc1 : NULL);
    (void)memcard_reload_bound();
    if (g_np.personal_save_dir[0])
        savestate_configure(g_np.personal_save_dir, g_np.bios_checksum, g_np.entry_pc);
    g_np.guest_sandbox = 0;
}

static void np_apply_ready_state(void)
{
    rnet_u8 op = 0, slot = 0;
    const void *data = NULL;
    size_t size = 0;

    if (!rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size))
        return;
    if (!data || size == 0) {
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        if (g_np.local_slot != 0 && np_apply_mc_blob((const uint8_t *)data, size) != 0) {
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        g_np.mc_sync_done = 1;
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        g_np.needs_advance = 0;
        g_np.latched_for_tick = 0;
        return;
    }

    if (op == RNET_STATE_OP_SAVE) {
        if (g_np.local_slot != 0) {
            if (!savestate_write_slot((int)slot, data, size)) {
                printf("psxrecomp: netplay guest save slot=%u — write failed\n", (unsigned)slot);
                fflush(stdout);
                rnet_session_state_finish(g_np.session, 0);
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            /* Post-transfer hash verify against wire CRC. */
            {
                uint32_t got_sz = 0, got_crc = 0;
                if (!np_slot_crc((int)slot, &got_sz, &got_crc) ||
                    got_sz != (uint32_t)size ||
                    got_crc != rnet_checksum((const rnet_u8 *)data, size)) {
                    printf("psxrecomp: netplay guest save slot=%u — post-CRC mismatch\n",
                           (unsigned)slot);
                    fflush(stdout);
                    rnet_session_state_finish(g_np.session, 0);
                    g_np.xfer = NP_XFER_NONE;
                    return;
                }
            }
            printf("psxrecomp: netplay guest save slot=%u — synced (%zu bytes)\n",
                   (unsigned)slot, size);
            fflush(stdout);
        } else {
            printf("psxrecomp: netplay save slot=%u — transfer complete\n", (unsigned)slot);
            fflush(stdout);
        }
        rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        return;
    }

    /* LOAD transfer (hash miss): guest stages the wire blob in memory (no disk
     * dependency — relative sandbox/CWD issues used to fail write_slot here).
     * Both peers request apply so host cannot restore before guest has bytes. */
    if (g_np.local_slot != 0) {
        if (!savestate_request_load_blob_protocol(data, size)) {
            printf("psxrecomp: netplay guest load slot=%u — blob stage failed "
                   "(%zu bytes, sandbox='%s')\n",
                   (unsigned)slot, size, savestate_dir());
            fflush(stdout);
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        /* Best-effort mirror to sandbox for hash-probe hits on rematch. */
        if (!savestate_write_slot((int)slot, data, size)) {
            printf("psxrecomp: netplay guest load slot=%u — sandbox mirror "
                   "failed (in-memory apply continues)\n",
                   (unsigned)slot);
            fflush(stdout);
        }
    } else {
        (void)savestate_request_load_protocol((int)slot);
    }
    rnet_session_state_finish(g_np.session, 0);
    np_begin_load_apply((int)slot);
    printf("psxrecomp: netplay load slot=%u — applying after transfer…\n", (unsigned)slot);
    fflush(stdout);
}

static void np_guest_handle_probe(void)
{
    rnet_u8 op = 0, slot = 0;
    rnet_u32 size = 0, crc = 0;
    int match = 0;

    if (g_np.local_slot == 0) return;
    if (!rnet_session_state_probe_pending(g_np.session, &op, &slot, &size, &crc))
        return;

    /* Post-load ready rendezvous (must be before SAVE size==0 coord). */
    if (op == RNET_STATE_OP_LOAD && size == 0 && crc == NP_LOAD_READY_CRC) {
        if (g_np.xfer == NP_XFER_LOAD_APPLYING) {
            if (savestate_pending())
                return; /* still need guest cycles to apply */
            if (savestate_take_load_completed()) {
                np_enter_load_ready((int)slot);
            } else if (!g_np.load_applied_local) {
                return; /* staged but not yet applied */
            }
        }
        if (g_np.xfer != NP_XFER_LOAD_READY && !g_np.load_applied_local) {
            return;
        }
        /* ACK host ready, commit resync+prime once, then stay in LOAD_READY
         * until try_admit succeeds (host must probe_finish first). */
        if (rnet_session_state_probe_reply(g_np.session, 1) != 0)
            return;
        np_commit_load_sync();
        if (!g_np.load_ready_replied) {
            g_np.load_ready_replied = 1;
            printf("psxrecomp: netplay load slot=%u — ready acked, waiting lockstep…\n",
                   (unsigned)slot);
            fflush(stdout);
        }
        return;
    }

    if (size == 0) {
        /* SAVE coord: crc carries the shared target sim_tick. Both peers
         * stage the write when sim reaches that tick (see
         * np_maybe_stage_target_save) so CRCs match and skip transfer. */
        if (g_np.xfer != NP_XFER_SAVE_COORD) {
            g_np.xfer = NP_XFER_SAVE_COORD;
            g_np.xfer_slot = (int)slot;
            g_np.save_target_tick = crc;
            g_np.local_save_staged = 0;
            g_np.local_save_acked = 0;
            printf("psxrecomp: netplay guest save slot=%u — armed target "
                   "sim=%u\n",
                   (unsigned)slot, (unsigned)crc);
            fflush(stdout);
        }
        if (savestate_pending()) return;
        if (!g_np.local_save_staged || !savestate_slot_exists((int)slot)) return;
        if (!g_np.local_save_acked) {
            g_np.local_save_acked = 1;
            (void)rnet_session_state_probe_reply(g_np.session, 1);
            printf("psxrecomp: netplay guest save slot=%u — local write done "
                   "@ target sim (frozen until hash probe)\n",
                   (unsigned)slot);
            fflush(stdout);
        }
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        uint32_t local_sz = 0, local_crc = 0;
        match = np_mc_blob_crc(&local_sz, &local_crc) && local_sz == size && local_crc == crc;
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (match) g_np.mc_sync_done = 1;
        return;
    }

    {
        uint32_t local_sz = 0, local_crc = 0;
        char reason[192];
        match = np_slot_crc((int)slot, &local_sz, &local_crc) && local_sz == size &&
                local_crc == crc;
        /* CRC match of a stale .pst (wrong codegen) is not loadable — ask the
         * host to transfer. Host also refuses probe start if its own slot is
         * stale, so this mainly covers guest-sandbox drift. */
        if (match && op == RNET_STATE_OP_LOAD &&
            !savestate_slot_compatible((int)slot, reason, sizeof(reason))) {
            printf("psxrecomp: netplay guest load slot=%u — hash matched but "
                   "unloadable (%s); requesting transfer\n",
                   (unsigned)slot, reason[0] ? reason : "incompatible");
            fflush(stdout);
            match = 0;
        }
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (op == RNET_STATE_OP_SAVE) {
            if (match) {
                g_np.xfer = NP_XFER_NONE;
                printf("psxrecomp: netplay guest save slot=%u — hashes match, "
                       "skip transfer\n",
                       (unsigned)slot);
                fflush(stdout);
            } else {
                /* Host will chunk the authoritative .pst — stay parked. */
                g_np.xfer = NP_XFER_SAVE_SEND;
                g_np.xfer_slot = (int)slot;
            }
        } else if (op == RNET_STATE_OP_LOAD) {
            if (match) {
                if (g_np.xfer != NP_XFER_LOAD_APPLYING &&
                    g_np.xfer != NP_XFER_LOAD_READY) {
                    (void)savestate_request_load_protocol((int)slot);
                    np_begin_load_apply((int)slot);
                    printf("psxrecomp: netplay guest load slot=%u — hashes match, "
                           "applying…\n",
                           (unsigned)slot);
                    fflush(stdout);
                }
            } else {
                /* Must mark LOAD_SEND or guest keeps the 20s admit timeout and
                 * BYEs the host mid-TURN transfer. */
                g_np.xfer = NP_XFER_LOAD_SEND;
                g_np.xfer_slot = (int)slot;
                printf("psxrecomp: netplay guest load slot=%u — hash miss, "
                       "waiting for transfer…\n",
                       (unsigned)slot);
                fflush(stdout);
            }
        }
    }
}

static void np_host_drive_xfer(void)
{
    int match = 0;
    uint32_t size = 0, crc = 0;
    uint8_t *buf = NULL;
    size_t n = 0;

    if (g_np.local_slot != 0 || !g_np.session) return;

    switch (g_np.xfer) {
    case NP_XFER_MC_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            g_np.mc_sync_done = 1;
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        {
            uint8_t *blob = (uint8_t *)malloc(NP_MC_BLOB_BYTES);
            size_t sz = 0;
            if (!blob || np_build_mc_blob(blob, NP_MC_BLOB_BYTES, &sz) != 0 ||
                rnet_session_state_begin(g_np.session, RNET_STATE_OP_SRAM, 0, blob, sz) != 0) {
                free(blob);
                g_np.mc_sync_done = 1;
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            free(blob);
            g_np.xfer = NP_XFER_MC_SEND;
        }
        return;

    case NP_XFER_SAVE_COORD:
        /* Host + guest both stage at save_target_tick; wait for local write
         * and guest ACK before hashing. */
        if (savestate_pending()) return;
        if (!g_np.local_save_staged || !savestate_slot_exists(g_np.xfer_slot))
            return;
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (!match) {
            /* Guest failed to save — still ship host blob. */
        }
        if (!np_slot_crc(g_np.xfer_slot, &size, &crc) ||
            rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, size,
                                     crc) != 0) {
            printf("psxrecomp: netplay save slot=%d — hash probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — hash probe (%u bytes)\n", g_np.xfer_slot,
               (unsigned)size);
        fflush(stdout);
        g_np.xfer = NP_XFER_SAVE_PROBE;
        return;

    case NP_XFER_SAVE_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            printf("psxrecomp: netplay save slot=%d — hashes match, skip transfer\n",
                   g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf ||
            rnet_session_state_begin(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            printf("psxrecomp: netplay save slot=%d — transfer begin failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        printf("psxrecomp: netplay save slot=%d — transferring %zu bytes to guest\n",
               g_np.xfer_slot, n);
        fflush(stdout);
        free(buf);
        g_np.xfer = NP_XFER_SAVE_SEND;
        return;

    case NP_XFER_LOAD_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            (void)savestate_request_load_protocol(g_np.xfer_slot);
            np_begin_load_apply(g_np.xfer_slot);
            printf("psxrecomp: netplay load slot=%d — hashes match, applying…\n",
                   g_np.xfer_slot);
            fflush(stdout);
            return;
        }
        if (!savestate_read_slot(g_np.xfer_slot, &buf, &n) || !buf) {
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        /* Do not stage savestate_request_load here — host would apply during
         * SEND, enter LOAD_READY, and suppress INPUT before the guest can
         * admit frames for its own savestate_poll (deadlock). Both peers
         * stage in np_apply_ready_state when the transfer completes. */
        g_np.load_applied_local = 0;
        g_np.load_sync_done = 0;
        if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, buf,
                                     n) != 0) {
            free(buf);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        free(buf);
        printf("psxrecomp: netplay load slot=%d — transferring %zu bytes\n", g_np.xfer_slot, n);
        fflush(stdout);
        g_np.xfer = NP_XFER_LOAD_SEND;
        return;

    case NP_XFER_MC_SEND:
    case NP_XFER_SAVE_SEND:
    case NP_XFER_LOAD_SEND:
        /* apply_ready runs first and clears take_ready (LOAD → LOAD_APPLYING). */
        if (rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL)) {
            if (g_np.xfer == NP_XFER_MC_SEND)
                g_np.mc_sync_done = 1;
            rnet_session_state_finish(g_np.session, 0);
            if (g_np.xfer == NP_XFER_LOAD_SEND) {
                np_begin_load_apply(g_np.xfer_slot);
            } else {
                g_np.xfer = NP_XFER_NONE;
            }
        }
        return;

    case NP_XFER_LOAD_READY:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        /* Mutual ready: drop probe stall first, then resync+prime. Stay in
         * LOAD_READY until try_admit (do not drop the app barrier early). */
        rnet_session_state_probe_finish(g_np.session);
        np_commit_load_sync();
        g_np.load_ready_replied = 1;
        printf("psxrecomp: netplay load slot=%d — mutual ready, waiting lockstep…\n",
               g_np.xfer_slot);
        fflush(stdout);
        return;

    default:
        return;
    }
}

static void np_prime_after_hard_resync(void)
{
    uint8_t bytes[PSX_NETPLAY_PAD_BYTES];
    PsxNetPad pad;

    /* Prime delay prefix with the current local hold (not forced neutral) so the
     * first D play frames continue what the player is already pressing. Each
     * peer only primes its own slot — lockstep stays valid. Tip latency for
     * *changes* remains D; we just avoid a post-load dead zone of released pads. */
    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFFFFu;
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
    pad.analog = 1;
    pad.connected = 1;
    if (g_np.staged_valid)
        pad = g_np.staged;
    pad.connected = 1;
    psx_netplay_normalize_pad(&pad);

    bytes[0] = (uint8_t)(pad.buttons & 0xFFu);
    bytes[1] = (uint8_t)((pad.buttons >> 8) & 0xFFu);
    bytes[2] = pad.lx;
    bytes[3] = pad.ly;
    bytes[4] = pad.rx;
    bytes[5] = pad.ry;
    bytes[6] = pad.analog ? 1u : 0u;
    bytes[7] = 1u;
    rnet_session_prime_delay_inputs(g_np.session, bytes, (rnet_u16)PSX_NETPLAY_PAD_BYTES);

    /* Keep staged matching the prime so the first tip sample is not a sudden
     * release while [0..D) still holds the live pad. */
    g_np.staged = pad;
    g_np.staged_valid = 1;
}

/* Stage restore. Keep INPUT flowing so try_admit can still run guest cycles
 * for savestate_poll — suppress only at mutual ready (np_commit_load_sync).
 * Ready probe must also leave INPUT unstalled (recomp-net size==0 LOAD). */
static void np_begin_load_apply(int slot)
{
    /* Transfer admit failures (state_xfer) often latch starvation; lead can sit
     * at D-1 after ICE xfer and would block the only frame savestate_poll needs. */
    np_starv_reset();
    g_np.xfer = NP_XFER_LOAD_APPLYING;
    g_np.load_applied_local = 0;
    g_np.load_sync_done = 0;
    g_np.load_ready_replied = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.live_valid = 0;
    g_np.xfer_slot = slot;
}

/* Once per load, at mutual ready (guest READY ACK / host take_reply). */
static void np_commit_load_sync(void)
{
    if (g_np.load_sync_done || !g_np.session)
        return;
    /* Suppress empty tips only for the hard_resync→prime window. */
    rnet_session_set_input_send_suppress(g_np.session, 1);
    rnet_session_hard_resync(g_np.session);
    np_prime_after_hard_resync(); /* clears suppress + emits fresh tip */
    netplay_hc_reset(&g_np.hc);
    np_part_ring_reset();
    g_np.load_sync_done = 1;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    /* staged_valid left set by prime — tip must match delay-prefix hold. */
}

static void np_enter_load_ready(int slot)
{
    /* Do not hard_resync/prime here — the later-applying peer would clear the
     * earlier peer's tip and stall resume. Sync runs at mutual ready.
     * Do not suppress INPUT here either: the first peer to finish apply must
     * keep sending pads so the other can still admit frames for savestate_poll. */
    g_np.load_applied_local = 1;
    g_np.load_ready_replied = 0;
    g_np.load_sync_done = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.live_valid = 0;
    g_np.xfer = NP_XFER_LOAD_READY;
    g_np.xfer_slot = slot;
}

/* After both peers stage a load: run until restore completes, then rendezvous.
 * hard_resync+prime happens once at mutual ready (not at apply). */
static void np_drive_load_barrier(void)
{
    if (g_np.xfer != NP_XFER_LOAD_APPLYING)
        return;
    if (savestate_pending())
        return;
    if (savestate_take_load_failed()) {
        /* Stale/mismatched .pst: do not sit in load_apply_done forever. */
        printf("psxrecomp: netplay load slot=%d — apply failed "
               "(incompatible or missing .pst) — aborting barrier\n",
               g_np.xfer_slot);
        fflush(stdout);
        if (g_np.session)
            rnet_session_state_finish(g_np.session, 0);
        g_np.xfer = NP_XFER_NONE;
        g_np.load_applied_local = 0;
        g_np.load_ready_replied = 0;
        g_np.load_sync_done = 0;
        g_np.load_apply_failed = 1;
        if (g_np.session)
            rnet_session_set_input_send_suppress(g_np.session, 0);
        return;
    }
    if (!g_np.load_applied_local && !savestate_take_load_completed())
        return;

    np_enter_load_ready(g_np.xfer_slot);

    if (g_np.local_slot == 0) {
        if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)g_np.xfer_slot, 0,
                                     NP_LOAD_READY_CRC) != 0) {
            printf("psxrecomp: netplay load slot=%d — ready probe failed\n", g_np.xfer_slot);
            fflush(stdout);
            g_np.xfer = NP_XFER_NONE;
            g_np.load_applied_local = 0;
            if (g_np.session)
                rnet_session_set_input_send_suppress(g_np.session, 0);
            return;
        }
        printf("psxrecomp: netplay load slot=%d — applied, waiting for guest…\n",
               g_np.xfer_slot);
        fflush(stdout);
    } else {
        printf("psxrecomp: netplay guest load slot=%d — applied, waiting for host…\n",
               g_np.xfer_slot);
        fflush(stdout);
    }
}

static void np_maybe_start_mc_sync(void)
{
    uint32_t size = 0, crc = 0;
    if (g_np.local_slot != 0 || g_np.mc_sync_sent || g_np.mc_sync_done)
        return;
    if (!rnet_session_is_running(g_np.session)) return;
    if (np_xfer_busy()) return;
    if (!np_mc_blob_crc(&size, &crc)) {
        g_np.mc_sync_done = 1;
        return;
    }
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SRAM, 0, size, crc) != 0) {
        g_np.mc_sync_done = 1;
        return;
    }
    g_np.mc_sync_sent = 1;
    g_np.xfer = NP_XFER_MC_PROBE;
}

static void encode_pad(const PsxNetPad *pad, RNetInputSample *out, rnet_u32 tick)
{
    PsxNetPad n = *pad;
    psx_netplay_normalize_pad(&n);
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->size = PSX_NETPLAY_PAD_BYTES;
    out->bytes[0] = (rnet_u8)(n.buttons & 0xFFu);
    out->bytes[1] = (rnet_u8)((n.buttons >> 8) & 0xFFu);
    out->bytes[2] = n.lx;
    out->bytes[3] = n.ly;
    out->bytes[4] = n.rx;
    out->bytes[5] = n.ry;
    out->bytes[6] = n.analog ? 1u : 0u;
    out->bytes[7] = 1u;
    out->valid = 1;
}

static void decode_pad(const RNetInputSample *in, PsxNetPad *pad)
{
    memset(pad, 0, sizeof(*pad));
    pad->buttons = 0xFFFFu;
    pad->lx = pad->ly = pad->rx = pad->ry = 0x80u;
    pad->analog = 1;
    pad->connected = 1;
    if (!in || !in->valid || in->size < PSX_NETPLAY_PAD_BYTES) return;
    pad->buttons = (uint16_t)in->bytes[0] | ((uint16_t)in->bytes[1] << 8);
    pad->lx = in->bytes[2];
    pad->ly = in->bytes[3];
    pad->rx = in->bytes[4];
    pad->ry = in->bytes[5];
    pad->analog = in->bytes[6] ? 1u : 0u;
    pad->connected = 1;
    psx_netplay_normalize_pad(pad);
}

static void apply_pad_slot(int slot, const PsxNetPad *pad)
{
    if (slot < 0 || slot >= g_np.slot_count || slot >= PSX_MAX_PLAYERS || !pad) return;
    const int on_tap = sio_pad_on_multitap(slot);
    sio_set_pad_connected(slot, 1);
    sio_set_pad_config_capable(slot, on_tap ? 0 : 1);
    sio_set_pad_state_slot(slot, pad->buttons);
    if (on_tap)
        sio_set_pad_sticks(slot, 0x80, 0x80, 0x80, 0x80);
    else
        sio_set_pad_sticks(slot, pad->lx, pad->ly, pad->rx, pad->ry);
    sio_request_pad_type(slot, (!on_tap && pad->analog) ? 1 : 0);
}

static void host_sample_local(rnet_u32 tick, RNetInputSample *out, void *ctx)
{
    NetplayState *st = (NetplayState *)ctx;
    PsxNetPad pad;
    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFFFFu;
    pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
    pad.analog = 1;
    pad.connected = 1;
    if (st->staged_valid) pad = st->staged;
    pad.connected = 1;
    encode_pad(&pad, out, tick);
}

static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *ctx)
{
    int i;
    int n;
    (void)tick;
    (void)ctx;
    if (!by_slot || slots <= 0) return;
    n = g_np.slot_count;
    if (n > slots) n = slots;
    if (n > PSX_MAX_PLAYERS) n = PSX_MAX_PLAYERS;
    force_session_pads_connected(n);
    for (i = 0; i < n; ++i) {
        PsxNetPad pad;
        decode_pad(&by_slot[i], &pad);
        apply_pad_slot(i, &pad);
    }
}

typedef struct {
    NetplayHashConfirm *hc;
    uint32_t            tick;
} NpHcGateCtx;

static uint8_t np_hash_confirm_promote_gate(void *ctx)
{
    NpHcGateCtx *g = (NpHcGateCtx *)ctx;
    if (!g || !g->hc) return 0;
    return netplay_hc_confirm_through(g->hc, g->tick);
}

static void np_publish_hist_sio(uint32_t tick)
{
    int i;
    force_session_pads_connected(g_np.slot_count);
    for (i = 0; i < g_np.slot_count && i < PSX_MAX_PLAYERS; ++i) {
        RNetRbFrame row;
        PsxNetPad pad;
        if (!netplay_ih_get(&g_np.ih, i, tick, &row))
            continue;
        netplay_ih_frame_to_pad(&row, &pad);
        apply_pad_slot(i, &pad);
    }
}

static void np_rb_apply_frame_slot(int slot, uint32_t tick, uint16_t buttons,
                                   int8_t sx, int8_t sy, uint8_t analog)
{
    RNetRbFrame row;
    PsxNetPad pad;
    (void)tick;
    memset(&row, 0, sizeof(row));
    row.tick = tick;
    row.buttons = buttons;
    row.stick_x = sx;
    row.stick_y = sy;
    row.analog = analog ? 1u : 0u;
    row.is_valid = 1;
    netplay_ih_frame_to_pad(&row, &pad);
    force_session_pads_connected(g_np.slot_count);
    apply_pad_slot(slot, &pad);
}

static void np_rb_bind_and_start(void)
{
    PsxNetplayRbBindings b;
    memset(&b, 0, sizeof(b));
    b.session = &g_np.session;
    b.cpu = &g_np.cpu;
    b.ih = &g_np.ih;
    b.hc = &g_np.hc;
    b.bios_checksum = &g_np.bios_checksum;
    b.entry_pc = &g_np.entry_pc;
    b.slot_count = &g_np.slot_count;
    b.local_slot = &g_np.local_slot;
    b.input_delay = &g_np.input_delay;
    b.publish_sio = np_publish_hist_sio;
    b.apply_frame_slot = np_rb_apply_frame_slot;
    psx_netplay_rb_bind(&b);
    psx_netplay_rb_start();
}

/* MotK digital (active-low): wire only releases buttons vs invent; sticks equal.
 * Used for FMV soft-promote releases only (menu releases must rewind). */
static int np_digital_release_only(const RNetInputContractFrame *pub,
                                   const RNetInputContractFrame *wire)
{
    uint16_t newly_pressed;
    uint16_t newly_released;
    if (!pub || !wire)
        return 0;
    if (pub->stick_x != wire->stick_x || pub->stick_y != wire->stick_y)
        return 0;
    if (pub->buttons == wire->buttons)
        return 0;
    newly_pressed = (uint16_t)((uint16_t)(~wire->buttons) & pub->buttons);
    newly_released = (uint16_t)((uint16_t)(~pub->buttons) & wire->buttons);
    return newly_pressed == 0 && newly_released != 0;
}

/* Wire has at least one newly pressed button vs published invent (active-low).
 * FMV skip uses this — promote-without-resim left host mid-movie while peer
 * already cut over. */
static int np_digital_new_press(const RNetInputContractFrame *pub,
                                const RNetInputContractFrame *wire)
{
    uint16_t newly_pressed;
    if (!pub || !wire)
        return 0;
    newly_pressed = (uint16_t)((uint16_t)(~wire->buttons) & pub->buttons);
    return newly_pressed != 0;
}

/* After promoting a digital release at `release_tick`, rewrite predicted hist
 * rows release_tick+1..end to the released pad. Hold-last invent ahead of the
 * release tip left `pub` still held → ghost second-release episodes (soak:
 * RIGHT release@1607 then another `pub=ffdf wire=ffff` @1634).
 *
 * §34: TipHold stalls sim at tip while coalesce promotes release at tip+N —
 * end must reach tip+runway, not only sim (sim <= release_tick was a no-op). */
static void np_scrub_ahead_predicted(int slot, rnet_u32 release_tick,
                                     const RNetRbFrame *released)
{
    rnet_u32 sim;
    rnet_u32 end;
    rnet_u32 t;
    unsigned n = 0;
    if (!released || !g_np.session)
        return;
    sim = rnet_session_sim_tick(g_np.session);
    end = sim;
    if (psx_netplay_rb_tip_holding()) {
        uint32_t tip = psx_netplay_rb_episode_target();
        uint32_t runway = psx_netplay_rb_tip_runway();
        uint32_t tip_end = tip + runway;
        if (tip_end > end)
            end = tip_end;
    }
    if (end <= release_tick)
        return;
    for (t = release_tick + 1u; t <= end; ++t) {
        RNetRbFrame row;
        RNetRbFrame scrub;
        if (!netplay_ih_get(&g_np.ih, slot, t, &row))
            continue;
        if (!row.is_predicted)
            continue;
        if (row.buttons == released->buttons &&
            row.stick_x == released->stick_x &&
            row.stick_y == released->stick_y)
            continue;
        scrub = *released;
        scrub.tick = t;
        scrub.is_predicted = 0u;
        scrub.is_valid = 1u;
        if (netplay_ih_promote(&g_np.ih, slot, &scrub))
            n++;
    }
    if (n) {
        fprintf(stderr,
                "psxrecomp: rb scrub-ahead release slot=%d from=%u end=%u "
                "sim=%u n=%u btn=%04x\n",
                slot, (unsigned)release_tick, (unsigned)end, (unsigned)sim, n,
                (unsigned)released->buttons);
        fflush(stderr);
    }
}

/* TipHold Live is stalled at invent-cap while digital is held — predicted hist
 * rows past tip may not exist yet, so ordinary reconcile never sees the
 * release. Peek wire tip+1..tip+runway; on the first pad delta vs tip hist,
 * promote the span and tip-extend (real coalesce). */
static void np_tip_hold_coalesce_ahead(void)
{
    uint32_t tip;
    uint32_t runway;
    rnet_u8 delay_u8;
    int slot;

    if (!g_np.rollback || !g_np.session)
        return;
    if (!psx_netplay_rb_tip_holding())
        return;
    if (psx_netplay_rb_load_pending() || psx_netplay_rb_is_resimulating())
        return;

    tip = psx_netplay_rb_episode_target();
    runway = psx_netplay_rb_tip_runway();
    if (tip == 0u || runway == 0u)
        return;

    delay_u8 = (rnet_u8)(g_np.input_delay < 0 ? 0
                        : (g_np.input_delay > 255 ? 255 : g_np.input_delay));

    for (slot = 0; slot < g_np.slot_count; ++slot) {
        RNetRbFrame tip_row;
        rnet_u32 edge = 0;
        rnet_u32 t;
        if (slot == g_np.local_slot)
            continue;
        if (!netplay_ih_get(&g_np.ih, slot, tip, &tip_row) || !tip_row.is_valid)
            continue;

        for (t = tip + 1u; t <= tip + runway; ++t) {
            RNetInputSample sample;
            PsxNetPad pad;
            RNetRbFrame wire_frame;
            rnet_u32 wire = rnet_wire_tick_from_sim(t, delay_u8);
            if (!rnet_session_peek_remote_input(g_np.session, slot, wire, &sample))
                break;
            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, t, 0, &wire_frame);
            if (wire_frame.buttons == tip_row.buttons &&
                wire_frame.stick_x == tip_row.stick_x &&
                wire_frame.stick_y == tip_row.stick_y)
                continue;
            edge = t;
            break;
        }
        if (!edge)
            continue;

        for (t = tip + 1u; t <= edge; ++t) {
            RNetInputSample sample;
            PsxNetPad pad;
            RNetRbFrame wire_frame;
            rnet_u32 wire = rnet_wire_tick_from_sim(t, delay_u8);
            if (!rnet_session_peek_remote_input(g_np.session, slot, wire, &sample))
                break;
            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, t, 0, &wire_frame);
            (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
        }
        {
            RNetInputSample sample;
            PsxNetPad pad;
            RNetRbFrame edge_frame;
            RNetInputContractFrame tip_c, edge_c;
            rnet_u32 wire = rnet_wire_tick_from_sim(edge, delay_u8);
            if (rnet_session_peek_remote_input(g_np.session, slot, wire, &sample)) {
                decode_pad(&sample, &pad);
                netplay_ih_pad_to_frame(&pad, edge, 0, &edge_frame);
                netplay_ih_frame_to_contract(&tip_row, &tip_c);
                netplay_ih_frame_to_contract(&edge_frame, &edge_c);
                if (np_digital_release_only(&tip_c, &edge_c))
                    np_scrub_ahead_predicted(slot, edge, &edge_frame);
            }
        }
        {
            static uint32_t s_last_log_tip, s_last_log_edge;
            static uint32_t s_coalesce_log_suppressed;
            if (tip != s_last_log_tip || edge != s_last_log_edge) {
                if (s_coalesce_log_suppressed) {
                    fprintf(stderr,
                            "psxrecomp: rb tip-hold coalesce-ahead "
                            "(suppressed %u repeats)\n",
                            (unsigned)s_coalesce_log_suppressed);
                    s_coalesce_log_suppressed = 0;
                }
                fprintf(stderr,
                        "psxrecomp: rb tip-hold coalesce-ahead tip=%u edge=%u "
                        "slot=%d tip_btn=%04x\n",
                        (unsigned)tip, (unsigned)edge, slot,
                        (unsigned)tip_row.buttons);
                fflush(stderr);
                s_last_log_tip = tip;
                s_last_log_edge = edge;
            } else {
                s_coalesce_log_suppressed++;
            }
        }
        if (psx_netplay_rb_tip_extend(edge, slot))
            return;
        /* tip-extend refused (span cap / different seat) — open via begin. */
        if (psx_netplay_rb_begin_rewind(edge, slot))
            return;
        /* Still tip-holding with an unreabsorbed wire edge: block quiet
         * finalize so we do not commit and drop the release (§34 soak). */
        psx_netplay_rb_tip_hold_block_quiet(1);
    }
}

static void np_timesync_note_late(uint32_t age);
static void np_timesync_note_mispredict(uint32_t age);

/* Late authoritative wire vs published predicted rows → promote or queue rewind.
 * Diag: promote-no-resim means hist took the late pad but guest sim did not
 * rewind — looks like "remote input rejected" when cadence already drifted. */
static void np_rollback_reconcile_wire(void)
{
    rnet_u32 sim;
    rnet_u32 t;
    int slot;
    int promote_sweep;
    int cooldown;
    int fmv_defer;
    int no_resim;
    /* Per-pump counters (one summary line when anything interesting happens). */
    unsigned n_no_resim = 0;
    unsigned n_soft_release = 0;
    unsigned n_contract_promote = 0;
    unsigned n_episode_open = 0;
    unsigned n_begin_refused = 0;
    rnet_u32 first_no_resim_t = 0;
    int first_no_resim_slot = -1;
    uint16_t first_pub_btn = 0;
    uint16_t first_wire_btn = 0;
    rnet_u32 first_rewind_t = 0;
    int first_rewind_slot = -1;
    const char *no_resim_why = NULL;
    RNetInputContractParams params;
    NpHcGateCtx gate_ctx;
    RNetInputContractHostGates gates;

    if (!g_np.rollback || !g_np.session) return;
    if (!rnet_session_is_running(g_np.session)) return;

    rnet_input_contract_params_init_defaults(&params);
    memset(&gates, 0, sizeof(gates));
    gate_ctx.hc = &g_np.hc;
    gates.ctx = &gate_ctx;
    gates.hash_confirm_promote = np_hash_confirm_promote_gate;

    promote_sweep = psx_netplay_rb_take_promote_sweep();
    cooldown = psx_netplay_rb_rewind_suppressed();
    fmv_defer = psx_netplay_rb_fmv_defer_rewind();
    {
        /* Media + short settle (§26): promote-only; admit waits for wire so
         * FMV Start/skip never opens depth24 tip episodes. Post-FMV menus
         * invent+resim (digest lockstep no longer gates admit). */
        int fmv_lock = psx_netplay_rb_lockstep_no_invent();
        no_resim = promote_sweep || cooldown || fmv_lock;
        if (promote_sweep)
            no_resim_why = "sweep";
        else if (cooldown)
            no_resim_why = "cooldown";
        else if (fmv_lock)
            no_resim_why = "fmv-lockstep";
    }
    sim = rnet_session_sim_tick(g_np.session);
    for (slot = 0; slot < g_np.slot_count; ++slot) {
        if (slot == g_np.local_slot) continue;
        for (t = (sim > 64u) ? (sim - 64u) : 0u; t <= sim; ++t) {
            RNetRbFrame published;
            RNetInputSample sample;
            RNetRbFrame wire_frame;
            RNetInputContractFrame pub_c, wire_c;
            RNetInputContractDecision d;
            uint8_t completed;
            PsxNetPad pad;
            rnet_u32 wire;
            rnet_u8 delay_u8;
            int pads_differ;

            if (!netplay_ih_get(&g_np.ih, slot, t, &published))
                continue;
            if (!published.is_predicted)
                continue;
            /* Hist is sim-keyed; tip rings are wire-keyed (sim + D). */
            delay_u8 = (rnet_u8)(g_np.input_delay < 0 ? 0
                                : (g_np.input_delay > 255 ? 255 : g_np.input_delay));
            wire = rnet_wire_tick_from_sim(t, delay_u8);
            if (!rnet_session_peek_remote_input(g_np.session, slot, wire, &sample))
                continue;

            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, t, 0, &wire_frame);
            netplay_ih_frame_to_contract(&published, &pub_c);
            netplay_ih_frame_to_contract(&wire_frame, &wire_c);
            pads_differ = (pub_c.buttons != wire_c.buttons) ||
                          (pub_c.stick_x != wire_c.stick_x) ||
                          (pub_c.stick_y != wire_c.stick_y);
            /* Feed the timesync pacer ONLY on a genuine mispredict (the
             * invented value was wrong). Firing on every predicted-row
             * resolution — the prior cut — meant EVERY normally-delayed
             * prediction counted as "late" (that is simply how input delay
             * works), drowning the one signal that actually identifies the
             * ahead peer. 2026-08-01 soak: gated correctly, the peer that
             * initiates corrections is unambiguous (28 vs 12 rewind-requests
             * split across the two sides in one session).
             * §32 lead regulation: also pass how many ticks this predicted
             * row rode before we caught it wrong (sim - t). A row resolved
             * right at the normal D-tick delay boundary is expected; one
             * that rode far longer means we were running unusually far
             * ahead of the confirmed remote tip when we guessed it — the
             * direct "how much lead did this mispredict cost us" signal
             * that scales the pacing debt below. */
            if (pads_differ)
                np_timesync_note_mispredict((sim >= t) ? (sim - t) : 0u);
            /* After commit/realign: flush invent poison without another episode. */
            if (no_resim) {
                (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                if (pads_differ) {
                    if (n_no_resim == 0) {
                        first_no_resim_t = t;
                        first_no_resim_slot = slot;
                        first_pub_btn = pub_c.buttons;
                        first_wire_btn = wire_c.buttons;
                    }
                    n_no_resim++;
                }
                continue;
            }
            gate_ctx.tick = t;
            completed = (sim > t) ? 1u : 0u;
            d = rnet_input_contract_stick_replace_decide(
                &pub_c, &wire_c, completed, &params, &gates);
            if (rnet_input_contract_decision_is_rewind(d)) {
                /* FMV/settle only: soft-promote releases (skip must rewind on
                 * press). Menu soft-promote + hold-last invent forked RAM
                 * (sticky Up skipped resim). Live invent is hold-last again —
                 * menu releases open a real episode. */
                if (fmv_defer && np_digital_release_only(&pub_c, &wire_c)) {
                    (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                    np_scrub_ahead_predicted(slot, t, &wire_frame);
                    if (n_soft_release == 0) {
                        first_no_resim_t = t;
                        first_no_resim_slot = slot;
                        first_pub_btn = pub_c.buttons;
                        first_wire_btn = wire_c.buttons;
                    }
                    n_soft_release++;
                    continue;
                }
                /* FMV: non-press pad noise → promote only (avoid CD thrash). */
                if (fmv_defer && !np_digital_new_press(&pub_c, &wire_c)) {
                    (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                    if (pads_differ) {
                        if (n_no_resim == 0) {
                            first_no_resim_t = t;
                            first_no_resim_slot = slot;
                            first_pub_btn = pub_c.buttons;
                            first_wire_btn = wire_c.buttons;
                            no_resim_why = "fmv-nopress";
                        }
                        n_no_resim++;
                    }
                    continue;
                }
                if (!g_np.pending_rewind) {
                    g_np.pending_rewind = 1;
                    g_np.pending_rewind_tick = t;
                    g_np.pending_rewind_slot = slot;
                    g_np.ih.rewind_count++;
                    if (g_np.rollback) {
                        if (first_rewind_slot < 0) {
                            first_rewind_t = t;
                            first_rewind_slot = slot;
                            first_pub_btn = pub_c.buttons;
                            first_wire_btn = wire_c.buttons;
                        }
                        /* Promote BEFORE begin — seal_inputs reads hist for the
                         * local seat and publish_sio falls back to hist before
                         * peer SEAL_ROWS land. Skipping promote left invent-idle
                         * pads sealed/published across the skip press tick. */
                        (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                        if (np_digital_release_only(&pub_c, &wire_c))
                            np_scrub_ahead_predicted(slot, t, &wire_frame);
                        if (psx_netplay_rb_begin_rewind(t, slot)) {
                            g_np.needs_advance = 0;
                            n_episode_open++;
                        } else {
                            n_begin_refused++;
                            if (psx_netplay_rb_tip_holding())
                                psx_netplay_rb_tip_hold_block_quiet(1);
                        }
                        g_np.pending_rewind = 0;
                    }
                }
            } else if (pads_differ) {
                (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
                if (np_digital_release_only(&pub_c, &wire_c))
                    np_scrub_ahead_predicted(slot, t, &wire_frame);
                n_contract_promote++;
            } else {
                (void)netplay_ih_promote(&g_np.ih, slot, &wire_frame);
            }
        }
    }

    /* TipHold invent-cap stall: peek wire past tip for release/press edges that
     * have no predicted hist row yet (Live never invented them). */
    if (!g_np.pending_rewind)
        np_tip_hold_coalesce_ahead();

    if (n_no_resim || n_soft_release || n_episode_open || n_begin_refused) {
        static uint32_t s_log_sim;
        static unsigned s_suppress;
        if (s_log_sim == sim && !n_episode_open) {
            s_suppress++;
        } else {
            if (s_suppress) {
                fprintf(stderr,
                        "psxrecomp: rb wire diag (+%u similar pumps suppressed)\n",
                        s_suppress);
                s_suppress = 0;
            }
            s_log_sim = sim;
            if (n_no_resim) {
                fprintf(stderr,
                        "psxrecomp: rb wire promote-no-resim sim=%u n=%u reason=%s "
                        "first_t=%u slot=%d pub=%04x wire=%04x "
                        "(hist ok; sim NOT rolled back — remote feels rejected)\n",
                        (unsigned)sim, n_no_resim,
                        no_resim_why ? no_resim_why : "?",
                        (unsigned)first_no_resim_t, first_no_resim_slot,
                        (unsigned)first_pub_btn, (unsigned)first_wire_btn);
            }
            if (n_soft_release) {
                fprintf(stderr,
                        "psxrecomp: rb wire soft-promote release-only sim=%u n=%u "
                        "first_t=%u slot=%d pub=%04x wire=%04x\n",
                        (unsigned)sim, n_soft_release,
                        (unsigned)first_no_resim_t, first_no_resim_slot,
                        (unsigned)first_pub_btn, (unsigned)first_wire_btn);
            }
            if (n_episode_open || n_begin_refused) {
                fprintf(stderr,
                        "psxrecomp: rb wire rewind-request sim=%u t=%u slot=%d "
                        "pub=%04x wire=%04x → episode_open=%u begin_refused=%u "
                        "contract_promote=%u\n",
                        (unsigned)sim, (unsigned)first_rewind_t, first_rewind_slot,
                        (unsigned)first_pub_btn, (unsigned)first_wire_btn,
                        n_episode_open, n_begin_refused, n_contract_promote);
            }
            fflush(stderr);
        }
    }
}

/* After episode commit: refuse invent until remote tip rebuilds most of D. */
static int g_cushion_rebuild;

/* Invent-grace RTT: POST-handshake EMA is asymmetric (often 0 on the peer
 * that receives POST first / light-tip skips). Always keep a D-scaled synth
 * floor so both peers share a comparable patience baseline; trusted raw can
 * only raise the estimate, never drop below the floor. */
static uint32_t np_invent_rtt_ms(uint32_t *raw_out)
{
    uint32_t raw = psx_netplay_rb_rtt_estimate_ms();
    uint32_t tick_ms = 17u;
    uint32_t delay_ticks;
    uint32_t synth;
    if (raw_out)
        *raw_out = raw;
    delay_ticks = (uint32_t)(g_np.input_delay > 0 ? g_np.input_delay : 4);
    synth = (delay_ticks * tick_ms) / 4u; /* ~1/4 of the delay runway in ms */
    if (synth < 8u)
        synth = 8u;
    if (synth > 24u)
        synth = 24u;
    if (raw >= 4u && raw > synth)
        return raw;
    return synth;
}

/* Stall-before-invent grace. Soaks showed every menu episode was a real
 * press/release edge whose wire row landed only 1–2 ticks after the seal
 * point (LAN, admit skew — not link latency): invent-on-first-miss then
 * mispredicted the edge and opened a paired episode every few ticks
 * (resim storm, 0.44–0.8x). Before inventing a missing remote row, stall
 * the admit up to PSX_RB_INVENT_GRACE_MS (default 30) measured from the
 * first miss of that wire tick. This is a rate governor, not just packet
 * wait: the lateness is sim skew (soak: ALL late edges were on the peer
 * running 1–2 ticks ahead; the other side had zero) — while the ahead
 * peer stalls here, the behind peer keeps simulating and catches up, so
 * in steady state the sims stay aligned and inputs arrive before the
 * seal with no stall at all. The first cut (8 ms) was shorter than one
 * tick of skew, expired on every edge and tripped the adaptive-off —
 * worst of both. The budget scales with the observed tick period: at
 * fight-scene rates (~35-45 fps) a tick lasts ~25 ms, so a fixed 30 ms
 * no longer covers 2 ticks of skew and gameplay stormed again; the
 * effective budget is max(base, 2.5x tick-period EMA), capped at 100 ms.
 * The caller only invokes this when >=2 ticks ahead of the remote sealed
 * tip: stalling on a 1-tick gap (normal pipeline phase offset) serialized
 * the two sims — each peer's admit waited out the other's guest frame and
 * throughput fell to 1/(g1+g2) even with zero episodes.
 * Adaptive off: if the grace keeps expiring (peer
 * genuinely lagging beyond the budget — real WAN latency), disable it
 * for 2s so we don't add a per-tick stall on top of real lag.
 * Host-side pacing only — the invented value is unchanged (hold-last), so
 * guest determinism is unaffected. 0 disables. */
/* Budget policy (2026-08-01, see docs/ROLLBACK_MOTK_HOOKUP.md section 12;
 * 2026-08-01 rb-diag follow-up): size from POST-handshake RTT when trusted
 * (>=4ms). When untrusted (guest often stuck at 0–1), use D-scaled synth
 * via np_invent_rtt_ms() so invent patience stays symmetric. */
/* budget_cap: hard ceiling on this stall (0 = disabled / invent now;
 * UINT32_MAX = full §21 budget for gap>=2). Gap=1 uses a short cap so the
 * ahead peer waits for an in-flight tip without re-serializing both sims
 * on a full peer guest frame (the 2026-07-31 40–50 fps regression).
 * count_expire: only gap>=2 expiries feed the adaptive-off streak — gap=1
 * fires constantly under phase stagger and must not disable the deep-gap
 * governor. */
static int np_invent_grace_stall_ex(int slot, rnet_u32 wire, uint32_t budget_cap,
                                    int count_expire)
{
    static int      s_grace_ms = -1;
    static rnet_u32 s_wire[PSX_MAX_PLAYERS];
    static uint32_t s_t0[PSX_MAX_PLAYERS];
    static uint8_t  s_expired[PSX_MAX_PLAYERS];
    static uint8_t  s_budget_logged[PSX_MAX_PLAYERS];
    static uint32_t s_expire_streak;
    static uint32_t s_off_until;
    static uint32_t s_tick_ema_ms; /* fallback-only EMA of inter-tick period, ms */
    uint32_t now;
    uint32_t budget;
    uint32_t rtt;
    uint32_t rtt_raw;
    uint32_t delay_ms = 0;

    if (s_grace_ms < 0) {
        const char *e = getenv("PSX_RB_INVENT_GRACE_MS");
        s_grace_ms = (e && e[0]) ? atoi(e) : 8;
        if (s_grace_ms < 0) s_grace_ms = 0;
        if (s_grace_ms > 200) s_grace_ms = 200;
        fprintf(stderr,
                "psxrecomp: rb invent grace floor=%d ms (minimum stall before "
                "hold-last invent; PSX_RB_INVENT_GRACE_MS — actual per-stall "
                "budget scales up from measured/synth RTT, see 'rb invent "
                "grace budget' lines; gap=1 uses a short cap, see §23)\n",
                s_grace_ms);
        fflush(stderr);
    }
    if (s_grace_ms == 0 || budget_cap == 0u || slot < 0 || slot >= PSX_MAX_PLAYERS)
        return 0;
    now = np_mono_ms();
    if (s_off_until != 0u && (int32_t)(now - s_off_until) < 0)
        return 0;
    s_off_until = 0u;
    if (s_wire[slot] != wire) {
        /* Previous tracked tick arrived before its grace expired (we moved
         * on without hitting the expiry path) — the link is keeping up. */
        if (s_wire[slot] != 0u && !s_expired[slot])
            s_expire_streak = 0;
        /* Consecutive tracked ticks give the live tick period — diagnostics
         * only; invent budget prefers np_invent_rtt_ms(). */
        if (s_wire[slot] != 0u && wire == s_wire[slot] + 1u) {
            uint32_t dt = now - s_t0[slot];
            if (dt >= 1u && dt <= 250u)
                s_tick_ema_ms = s_tick_ema_ms
                                    ? (3u * s_tick_ema_ms + dt) / 4u
                                    : dt;
        }
        s_wire[slot] = wire;
        s_t0[slot] = now;
        s_expired[slot] = 0;
        s_budget_logged[slot] = 0;
        return 1;
    }
    budget = (uint32_t)s_grace_ms;
    rtt = np_invent_rtt_ms(&rtt_raw);
    /* Ceiling scales with the configured input_delay (2026-08-01, see
     * docs/ROLLBACK_MOTK_HOOKUP.md §21). Half the nominal delay window
     * (floored so D<=4 keeps usable patience, capped so a very large D
     * can't hang admit indefinitely). */
    {
        uint32_t tick_ms = s_tick_ema_ms ? s_tick_ema_ms : 17u; /* ~59.94Hz nominal */
        uint32_t delay_ticks = (uint32_t)(g_np.input_delay > 0 ? g_np.input_delay : 0);
        uint32_t rtt_ceiling;
        delay_ms = delay_ticks * tick_ms;
        rtt_ceiling = delay_ms / 2u;
        /* §26: floor was 60ms — with D=4 synth RTT that forced a 25ms+
         * invent tax every RUNWAY_EMPTY miss and capped WAN at ~30fps even
         * after gap1 invent. Cap patience closer to one frame. */
        if (rtt_ceiling < 20u) rtt_ceiling = 20u;
        if (rtt_ceiling > 80u) rtt_ceiling = 80u;

        /* 1.5x invent RTT (trusted POST sample or D-scaled synth). */
        {
            uint32_t scaled = rtt + rtt / 2u;
            if (scaled > budget)
                budget = scaled;
            if (budget > rtt_ceiling)
                budget = rtt_ceiling;
        }
    }
    if (budget_cap != 0xffffffffu && budget > budget_cap)
        budget = budget_cap;
    if (!s_budget_logged[slot]) {
        s_budget_logged[slot] = 1;
        fprintf(stderr,
                "psxrecomp: rb invent grace budget=%u ms (floor=%d rtt=%u "
                "rtt_raw=%u tick_ema=%u delay_ms=%u cap=%u) slot=%d wire=%u\n",
                (unsigned)budget, s_grace_ms, (unsigned)rtt, (unsigned)rtt_raw,
                (unsigned)s_tick_ema_ms, (unsigned)delay_ms,
                (unsigned)budget_cap, slot, (unsigned)wire);
        fflush(stderr);
    }
    if ((uint32_t)(now - s_t0[slot]) < budget)
        return 1;
    if (!s_expired[slot]) {
        s_expired[slot] = 1;
        if (count_expire && ++s_expire_streak >= 15u) {
            s_expire_streak = 0;
            /* §26: WAN soaks needed invent-free sooner than 45×25ms (~2s of
             * admit tax before OFF). Hold OFF longer so FMV/menu cutovers
             * don't immediately re-arm a 25ms per-tick stall. */
            s_off_until = now + 5000u;
            fprintf(stderr,
                    "psxrecomp: rb invent grace OFF 5s (remote input "
                    "consistently later than %u ms)\n",
                    (unsigned)budget);
            fflush(stderr);
        }
    }
    return 0;
}

/* Gap=1 invent grace. §27 made this a flat 0 (invent on first miss) to kill
 * WAN delay-sync — but the 2026-08-01 LAN soak showed the *default* path
 * (no PSX_RB_GAP1_GRACE_MS override) was inventing on a rock-stable
 * remote_lead=1 hundreds of frames in a row: not starvation, a one-tick
 * phase offset between two sims that are otherwise keeping up. §28 splits
 * gap=1 into two cases instead of one flat policy:
 *   Case A (healthy/advancing tip): remote_lead>=1 and the tip has
 *     advanced recently relative to its own arrival cadence — the newest
 *     row is in flight, not missing. Wait up to the time remaining until
 *     the next expected tip advance (a few ms) instead of inventing.
 *   Case B (stale/starved tip): remote_lead<=0 or the tip hasn't advanced
 *     in over ~1.5x its cadence — genuinely nothing is coming soon.
 *     Invent immediately, same as §27's default, so WAN gaps stay
 *     responsive and this never turns back into cushion-wait.
 * PSX_RB_GAP1_GRACE_MS still forces the old flat-cap behavior for A/B
 * testing or an operator override; when unset, §28's adaptive split runs
 * instead of a fixed cap. */
static uint32_t g_gap1_shrink_until_ms;
static uint32_t g_gap1_expire_invent_streak;
#define RB_GAP1_SHRINK_CAP_MS 6u
#define RB_GAP1_SHRINK_HOLD_MS 1000u
/* §27: deep invent (pred_depth≥2) only after tip looks stale. */
#define RB_INVENT_DEPTH_STALE_FLOOR_MS 40u
#define RB_INVENT_RUNWAY_GRACE_CAP_MS 8u
/* §29: §28's Case A wait window (RB_GAP1_CASE_A_FLOOR_MS/CEIL_MS) was
 * removed — see the gap=1 branch of np_try_admit_rollback for why. The
 * A/B classification itself is kept (diagnostics only, zero latency):
 * tip considered "advancing" if its last advance was within this
 * multiple of its own arrival-period EMA (fixed-point x2, i.e. 1.5x). */
#define RB_TIP_FRESH_MULT_X2 3u

/* §28: tip arrival cadence — tracks how often the confirmed remote tip
 * (highest_remote_wire) actually advances, independent of whether admit
 * hit a miss. This lets a gap=1 miss be judged against "is a new row
 * about due" instead of a flat timeout: rb-diag soaks showed remote_lead
 * sitting at a stable 1 for hundreds of frames while the tip kept
 * advancing on a steady ~1-tick cadence — that is phase offset, not
 * starvation. */
static uint32_t g_tip_last_highest;
static uint32_t g_tip_last_advance_ms;
static uint32_t g_tip_arrival_ema_ms;
static uint8_t  g_tip_have_advance;

static void np_tip_track_advance(rnet_u32 highest_remote_wire)
{
    uint32_t now = np_mono_ms();

    if (!g_tip_have_advance) {
        g_tip_last_highest = highest_remote_wire;
        g_tip_last_advance_ms = now;
        g_tip_have_advance = 1;
        return;
    }
    if (highest_remote_wire == g_tip_last_highest)
        return;
    {
        uint32_t dt = now - g_tip_last_advance_ms;
        if (dt >= 1u && dt <= 250u)
            g_tip_arrival_ema_ms = g_tip_arrival_ema_ms
                                        ? (3u * g_tip_arrival_ema_ms + dt) / 4u
                                        : dt;
    }
    g_tip_last_highest = highest_remote_wire;
    g_tip_last_advance_ms = now;
}

/* ms since the remote tip last advanced; UINT32_MAX if never observed yet
 * (treated as stale — no evidence the pipeline is healthy). */
static uint32_t np_tip_age_ms(void)
{
    if (!g_tip_have_advance)
        return 0xffffffffu;
    return np_mono_ms() - g_tip_last_advance_ms;
}

/* has_override: set to 1 if PSX_RB_GAP1_GRACE_MS forces a flat cap (the
 * return value is that cap, already SHRINK-adjusted). Set to 0 when
 * unset — caller should run the §28 adaptive Case A/B split instead; the
 * return value is 0 and must be ignored. */
static uint32_t np_gap1_grace_cap_ms(int *has_override)
{
    static int s_cap = -2; /* -2 unset */
    uint32_t rtt_raw;
    uint32_t cap;
    uint32_t now;

    if (s_cap == -2) {
        const char *e = getenv("PSX_RB_GAP1_GRACE_MS");
        if (e && e[0]) {
            s_cap = atoi(e);
            if (s_cap < 0) s_cap = 0;
            if (s_cap > 40) s_cap = 40;
            fprintf(stderr,
                    "psxrecomp: rb gap1 invent grace cap=%d ms "
                    "(PSX_RB_GAP1_GRACE_MS override; §28 adaptive split "
                    "disabled)\n",
                    s_cap);
            fflush(stderr);
        } else {
            s_cap = -1; /* §28: no flat override, adaptive split decides */
            fprintf(stderr,
                    "psxrecomp: rb gap1 invent grace: adaptive §28 split "
                    "(healthy/advancing tip waits a few ms; stale tip "
                    "invents now; PSX_RB_GAP1_GRACE_MS forces a flat cap)\n");
            fflush(stderr);
        }
    }
    if (has_override)
        *has_override = (s_cap >= 0);
    if (s_cap < 0)
        return 0u;
    now = np_mono_ms();
    if (s_cap == 0)
        return 0u;
    cap = (uint32_t)s_cap;
    /* Only shrink when we have a trusted link sample — otherwise shrink
     * recreates the guest invent/host wait split. */
    rtt_raw = psx_netplay_rb_rtt_estimate_ms();
    if (rtt_raw < 4u) {
        g_gap1_shrink_until_ms = 0u;
    } else if (g_gap1_shrink_until_ms != 0u &&
               (int32_t)(now - g_gap1_shrink_until_ms) < 0) {
        if (cap > RB_GAP1_SHRINK_CAP_MS)
            cap = RB_GAP1_SHRINK_CAP_MS;
    } else {
        g_gap1_shrink_until_ms = 0u;
    }
    return cap;
}

/* Call after inventing at gap=1 when grace already expired for this wire. */
static void np_gap1_note_expire_invent(void)
{
    uint32_t now = np_mono_ms();
    /* No SHRINK while POST-RTT is untrusted — both peers must keep the same
     * invent patience (see rb-diag1/2 guest rtt=0–1 vs host 15–48). */
    if (psx_netplay_rb_rtt_estimate_ms() < 4u) {
        g_gap1_expire_invent_streak = 0u;
        return;
    }
    if (++g_gap1_expire_invent_streak < 10u)
        return;
    g_gap1_expire_invent_streak = 0u;
    g_gap1_shrink_until_ms = now + RB_GAP1_SHRINK_HOLD_MS;
    fprintf(stderr,
            "psxrecomp: rb gap1 invent grace SHRINK %ums for %ums "
            "(stall expired into invent repeatedly; trusted RTT only)\n",
            (unsigned)RB_GAP1_SHRINK_CAP_MS, (unsigned)RB_GAP1_SHRINK_HOLD_MS);
    fflush(stderr);
}

static void np_gap1_note_grace_helped(void)
{
    g_gap1_expire_invent_streak = 0u;
}

static int np_invent_grace_stall(int slot, rnet_u32 wire)
{
    return np_invent_grace_stall_ex(slot, wire, 0xffffffffu, 1);
}

/* Mispredict-driven timesync pacing. The first cut used a GGPO-style
 * advantage metric (wire_need - highest received remote row) with an
 * absolute +0.5 tick threshold — soak showed BOTH peers measure ~+0.6
 * ticks at the natural operating point (each side samples at the start
 * of its own tick), so both throttled, neither "closed", and both
 * tripped the off-guard while the mispredicts continued. The ground
 * truth for "I am the ahead peer" is the mispredict itself: only the
 * ahead side promotes real rows that arrived AFTER it already invented
 * them WRONG — note_late() is gated on pads_differ in the reconcile
 * caller (an earlier cut fired on every predicted-row resolution, which
 * is just normal input-delay operation and swamped the signal). Each
 * genuine mispredict adds ~half a tick of pacing debt (capped at 2
 * ticks); the admit path shaves the debt off at <=3 ms per tick. Zero
 * cost in steady state (aligned phase -> no mispredicts -> no debt).
 * Adaptive off: on real WAN transit every edge mispredicts no matter the
 * phase, so debt keeps landing back at the cap — a streak of 12
 * consecutive cap-hits (no room to have drained between them) disables
 * for 10 s. Earlier cut used "debt continuously nonzero for 5s", but an
 * active mispredict burst legitimately keeps debt elevated while pacing
 * is working exactly as intended — soak: 24/36 commits and 19/28
 * rewind-requests on the busier peer landed AFTER that off-guard fired,
 * i.e. it disabled pacing precisely when the storm needed it most. Host
 * pacing only — guest determinism unaffected. PSX_RB_TIMESYNC=0
 * disables. */
static int      g_ts_enabled = -1;
static uint32_t g_ts_tick_ema_ms;
static uint32_t g_ts_debt_ms;
static uint32_t g_ts_pegged_streak; /* consecutive mispredicts landing at/above cap */
static uint32_t g_ts_off_until_ms;
/* §30 soak: prove/disprove "suppressed pacing ↔ chronically ahead peer".
 * No algorithm change — counters only. Each peer logs its own view:
 * mispredicts = remote pads_differ resolves; note_late_applied = debt
 * actually added; note_late_suppressed_rb = note_late early-out while
 * rb_active/tip_holding (the hypothesized control-loop leak). */
static uint32_t g_ts_mispredict_count;
/* §32: cumulative/max prediction "age" (ticks ridden before a wrong guess
 * was caught) across mispredicts, and how much of that fed extra debt vs
 * the flat baseline — lets a soak confirm whether the peer with more
 * mispredicts is also the one running further ahead per-edge (the
 * over-prediction feedback loop fable's review flagged) rather than just
 * eating more edges at the same depth. */
static uint64_t g_ts_mispredict_age_sum;
static uint32_t g_ts_mispredict_age_max;
static uint32_t g_ts_note_late_applied;
static uint32_t g_ts_note_late_suppressed_rb;
static uint32_t g_ts_note_late_suppressed_off;
static uint32_t g_ts_debt_added_ms; /* cumulative debt added (pre-cap clamp) */
/* remote_lead samples taken every live admit (window reset on phase-ctrl log). */
static int64_t  g_ts_lead_sum;
static uint32_t g_ts_lead_n;
static int      g_ts_lead_min;
static int      g_ts_lead_max;
static int      g_ts_lead_have;

static void np_timesync_check_enabled(void)
{
    if (g_ts_enabled < 0) {
        const char *e = getenv("PSX_RB_TIMESYNC");
        g_ts_enabled = (e && e[0]) ? (atoi(e) != 0) : 1;
    }
}

void psx_netplay_timesync_on_episode_boundary(void)
{
    /* Resim/tip-hold can leave debt elevated; clear only the pegged-streak
     * off-guard so the next live mispredicts can re-arm pacing. Keep debt —
     * the ahead peer may still need a few ms/tick shaves after TipHold. */
    g_ts_pegged_streak = 0u;
    g_cushion_rebuild = 1;
}

/* Reconcile saw a real remote row that contradicted what we invented for
 * this tick — i.e. we are (locally) the mispredicting/ahead peer for this
 * edge. Add pacing debt.
 * Adaptive off (2026-08-01 soak): the first cut disabled after debt sat
 * "continuously nonzero" for 5s — but during an active mispredict burst,
 * debt SHOULD stay elevated for a while (each edge tops it back up before
 * the previous slice fully drains); that isn't pacing failing, it's pacing
 * doing its job under sustained load. Soak evidence: 24/36 episode commits
 * and 19/28 rewind-requests on the busier peer landed AFTER that off-guard
 * fired — it was disabling exactly when it was needed most. The real "this
 * is transit latency, not phase skew" signal is debt landing AT THE CAP
 * repeatedly with no room to have drained in between — track a streak of
 * cap-hits instead of wall-clock nonzero time. */
static void np_timesync_note_late(uint32_t age)
{
    uint32_t now;
    uint32_t add;
    uint32_t cap;
    uint32_t expected_age;
    uint32_t extra_age;

    np_timesync_check_enabled();
    if (!g_ts_enabled)
        return;
    /* Replay/tip-hold cost is not phase skew — do not feed pegged-streak
     * adaptive-off (fight/resim load used to look like "WAN transit").
     * §30: count these suppressions — hypothesis is the ahead peer spends
     * more time here and loses the correction signal that would slow it. */
    if (psx_netplay_rb_active() || psx_netplay_rb_tip_holding()) {
        g_ts_note_late_suppressed_rb++;
        return;
    }
    now = np_mono_ms();
    if (g_ts_off_until_ms != 0u && (int32_t)(now - g_ts_off_until_ms) < 0) {
        g_ts_note_late_suppressed_off++;
        return;
    }
    g_ts_off_until_ms = 0u;
    /* ~1.25 ticks per mispredict (was 0.75) — §29 soak: remote_lead sat at
     * a rock-stable D-1 for the entire match (LAN, hold-last suppresses
     * most gap1 misses from ever becoming a mispredict, so real edges are
     * the only signal this scheduler gets that one side is racing ahead).
     * With mispredicts this sparse, 0.75 ticks/edge could not close a
     * persistent 1-tick offset inside a match; push the per-edge closure
     * harder and let more debt accumulate (cap 3 ticks, was 2) so a
     * cluster of edges (a fight exchange) actually walks the phase back
     * to D instead of just taking the storm's mispredicts as its due. */
    add = g_ts_tick_ema_ms ? (g_ts_tick_ema_ms * 5u) / 4u : 20u;
    cap = g_ts_tick_ema_ms ? g_ts_tick_ema_ms * 3u : 50u;
    /* §32 lead regulation: a mispredict resolved right at the normal
     * input-delay boundary (age ≈ D) is expected steady-state noise — no
     * bonus, behavior unchanged from before this cut. A mispredict that
     * rode notably longer than D ticks before we caught it means we were
     * running unusually far ahead of the confirmed remote tip when we
     * guessed it; scale extra debt by how far past D it went (+25% of the
     * base add per extra tick of age, still bounded by the existing cap
     * below) so the peer that is actually accumulating lead brakes harder,
     * proportional to how far ahead it got, instead of every edge costing
     * the same flat debt regardless of how deep the guess was. */
    expected_age = g_np.input_delay > 0 ? (uint32_t)g_np.input_delay : 2u;
    extra_age = (age > expected_age) ? (age - expected_age) : 0u;
    if (extra_age)
        add += (add * extra_age) / 4u;
    /* 18 consecutive cap-hits (was 12): fight scenes can peg briefly while
     * pacing is still working; require a longer streak before declaring
     * transit latency. */
    if (g_ts_debt_ms >= cap) {
        if (++g_ts_pegged_streak >= 18u) {
            g_ts_pegged_streak = 0u;
            g_ts_debt_ms = 0u;
            g_ts_off_until_ms = now + 10000u;
            fprintf(stderr,
                    "psxrecomp: rb timesync OFF 10s (mispredicts keep landing "
                    "at the pacing cap — transit latency, not phase skew)\n");
            fflush(stderr);
            return;
        }
    } else {
        g_ts_pegged_streak = 0u;
    }
    g_ts_debt_ms += add;
    if (g_ts_debt_ms > cap)
        g_ts_debt_ms = cap;
    g_ts_note_late_applied++;
    g_ts_debt_added_ms += add;
}

static void np_timesync_note_mispredict(uint32_t age)
{
    g_ts_mispredict_count++;
    /* Raw signal (age-at-catch), independent of whether debt was actually
     * applied below — a suppressed edge (resim/tip-hold/off-guard) still
     * tells us how far ahead this peer was running when it guessed wrong. */
    g_ts_mispredict_age_sum += age;
    if (age > g_ts_mispredict_age_max)
        g_ts_mispredict_age_max = age;
    np_timesync_note_late(age);
}

static void np_timesync_sample_lead(int remote_lead)
{
    if (!g_ts_lead_have) {
        g_ts_lead_min = remote_lead;
        g_ts_lead_max = remote_lead;
        g_ts_lead_have = 1;
    } else {
        if (remote_lead < g_ts_lead_min)
            g_ts_lead_min = remote_lead;
        if (remote_lead > g_ts_lead_max)
            g_ts_lead_max = remote_lead;
    }
    g_ts_lead_sum += remote_lead;
    g_ts_lead_n++;
}

/* §30: ~1 Hz phase-control soak line. Compare host vs guest:
 * higher lead + higher mispredicts + higher suppressed_rb on one peer
 * ⇒ control-loop instability; balanced/rare suppress ⇒ gameplay/transport. */
static void np_phase_ctrl_maybe_log(uint32_t now, rnet_u32 sim, int remote_lead)
{
    static uint32_t s_last;
    int lead_avg;

    if (s_last != 0u && (uint32_t)(now - s_last) < 1000u)
        return;
    s_last = now ? now : 1u;
    lead_avg = g_ts_lead_n ? (int)(g_ts_lead_sum / (int64_t)g_ts_lead_n)
                           : remote_lead;
    fprintf(stderr,
            "psxrecomp: rb phase ctrl slot=%d sim=%u lead=%d lead_avg=%d "
            "lead_min=%d lead_max=%d debt_ms=%u debt_added=%u "
            "mispredict=%u mispredict_age_avg=%u mispredict_age_max=%u "
            "note_late=%u suppressed_rb=%u suppressed_off=%u "
            "D=%d\n",
            g_np.local_slot, (unsigned)sim, remote_lead, lead_avg,
            g_ts_lead_have ? g_ts_lead_min : remote_lead,
            g_ts_lead_have ? g_ts_lead_max : remote_lead,
            (unsigned)g_ts_debt_ms, (unsigned)g_ts_debt_added_ms,
            (unsigned)g_ts_mispredict_count,
            (unsigned)(g_ts_mispredict_count
                           ? (g_ts_mispredict_age_sum / g_ts_mispredict_count)
                           : 0u),
            (unsigned)g_ts_mispredict_age_max,
            (unsigned)g_ts_note_late_applied,
            (unsigned)g_ts_note_late_suppressed_rb,
            (unsigned)g_ts_note_late_suppressed_off,
            g_np.input_delay);
    fflush(stderr);
    /* Windowed lead stats reset each second; cumulative counters keep rising. */
    g_ts_lead_sum = 0;
    g_ts_lead_n = 0;
    g_ts_lead_have = 0;
}

/* Admit-side: shave pacing debt off at <=6 ms per tick (§29: was 4; §23:
 * was 3). Sparse mispredicts (hold-last suppresses most gap1 misses from
 * ever mispredicting) meant debt drained faster than it could accumulate
 * enough to matter — a bigger per-tick shave lets a handful of edges pull
 * a stuck D-1 phase back to D within a few ticks instead of dozens. */
static int np_timesync_throttle(uint32_t wire)
{
    static uint32_t s_last_wire;
    static uint32_t s_last_wire_ms;
    static uint32_t s_stall_until;
    static uint8_t  s_logged;
    uint32_t now;

    np_timesync_check_enabled();
    if (!g_ts_enabled)
        return 0;
    now = np_mono_ms();
    if (wire != s_last_wire) {
        if (s_last_wire != 0u && wire == s_last_wire + 1u) {
            uint32_t dt = now - s_last_wire_ms;
            if (dt >= 1u && dt <= 250u)
                g_ts_tick_ema_ms = g_ts_tick_ema_ms
                                       ? (7u * g_ts_tick_ema_ms + dt) / 8u
                                       : dt;
        }
        s_last_wire = wire;
        s_last_wire_ms = now;
        if (g_ts_debt_ms > 0u) {
            uint32_t slice = g_ts_debt_ms > 6u ? 6u : g_ts_debt_ms;
            g_ts_debt_ms -= slice;
            if (g_ts_debt_ms == 0u)
                s_logged = 0;
            s_stall_until = now + slice;
            if (!s_logged) {
                fprintf(stderr,
                        "psxrecomp: rb timesync pacing (debt=%u ms tick=%u ms — "
                        "shaving <=6 ms/tick)\n",
                        (unsigned)(g_ts_debt_ms + slice),
                        (unsigned)g_ts_tick_ema_ms);
                fflush(stderr);
                s_logged = 1;
            }
        }
    }
    if (s_stall_until != 0u && (int32_t)(now - s_stall_until) < 0)
        return 1;
    s_stall_until = 0u;
    return 0;
}

/* Admit telemetry (§22/§23): invents by gap size + P-cap freeze streak. */
static uint32_t g_admit_invent_gap1;
static uint32_t g_admit_invent_gap2;
static uint32_t g_admit_invent_gap3p;
static uint32_t g_admit_gap1_grace; /* times gap=1 short grace returned stall */
static uint32_t g_admit_pcap_stalls;
static uint32_t g_admit_pcap_enters;
static int      g_pcap_frozen;
static uint32_t g_pcap_freeze_enters_window;
static uint32_t g_pcap_window_t0_ms;
static uint32_t g_adapt_last_bump_ms;
static uint32_t g_admit_stats_last_log_ms;
/* Soak-only: PSX_RB_ADAPT_DELAY=0 disables mid-match D bumps. */
static int      g_adapt_delay_enabled = -1;

#define RB_ADAPT_FREEZE_ENTERS_THRESH 3u
#define RB_ADAPT_WINDOW_MS            5000u
#define RB_ADAPT_COOLDOWN_MS          10000u
#define RB_ADAPT_DELAY_MAX            16

static void np_sync_input_delay_from_session(void)
{
    rnet_u8 d;
    if (!g_np.session)
        return;
    d = rnet_session_committed_delay(g_np.session);
    if (d >= 2u && (int)d != g_np.input_delay) {
        fprintf(stderr,
                "psxrecomp: rb delay committed %d → %u (session)\n",
                g_np.input_delay, (unsigned)d);
        fflush(stderr);
        g_np.input_delay = (int)d;
    } else if (d >= 2u) {
        g_np.input_delay = (int)d;
    }
}

static void np_admit_note_invent_gap(rnet_u32 wire, rnet_u32 highest_remote)
{
    rnet_u32 gap;
    if (wire <= highest_remote)
        gap = 0u;
    else
        gap = wire - highest_remote;
    if (gap <= 1u)
        g_admit_invent_gap1++;
    else if (gap == 2u)
        g_admit_invent_gap2++;
    else
        g_admit_invent_gap3p++;
}

/* Why we spent prediction budget (one line per invent; throttle bursts). */
static uint32_t g_admit_invent_runway_empty;
static uint32_t g_admit_invent_tip_stale;
static uint32_t g_admit_invent_gap1_legacy;
static uint32_t g_admit_cushion_wait; /* lead>0 stalls that refused invent */
/* §28: gap1 Case A/B split telemetry — Case A grants (or attempts) a short
 * adaptive wait because the tip looked healthy/advancing; Case B invents
 * immediately because the tip looked stale or remote_lead was <=0. */
static uint32_t g_admit_gap1_case_a;
static uint32_t g_admit_gap1_case_b;

static void np_admit_log_invent(rnet_u32 sim, rnet_u32 wire,
                                rnet_u32 highest_remote, int remote_lead,
                                const char *reason)
{
    static uint32_t s_last_ms;
    static uint32_t s_burst;
    uint32_t now = np_mono_ms();
    uint32_t pred_depth =
        (wire > highest_remote) ? (wire - highest_remote) : 0u;
    int D = g_np.input_delay > 0 ? g_np.input_delay : 0;

    if (s_last_ms != 0u && (uint32_t)(now - s_last_ms) < 50u) {
        s_burst++;
        if ((s_burst & 15u) != 0u)
            return;
    } else {
        s_burst = 0u;
    }
    s_last_ms = now ? now : 1u;
    fprintf(stderr,
            "psxrecomp: rb invent sim=%u wire=%u remote_tip=%u D=%d "
            "pred_depth=%u remote_lead=%d reason=%s%s\n",
            (unsigned)sim, (unsigned)wire, (unsigned)highest_remote, D,
            (unsigned)pred_depth, remote_lead, reason,
            s_burst ? " (burst)" : "");
    fflush(stderr);
}

static void np_admit_log_runway(uint32_t now, rnet_u32 sim, rnet_u32 wire,
                                rnet_u32 highest_remote, int remote_lead)
{
    static uint32_t s_last;
    uint32_t pred_depth;
    int runway_rem;
    uint32_t rtt_raw = 0;
    uint32_t rtt = np_invent_rtt_ms(&rtt_raw);
    int D = g_np.input_delay > 0 ? g_np.input_delay : 0;

    if (s_last != 0u && (uint32_t)(now - s_last) < 1000u)
        return;
    s_last = now ? now : 1u;
    pred_depth = (wire > highest_remote) ? (wire - highest_remote) : 0u;
    /* How many delay frames of remote tip remain vs live sim. */
    runway_rem = remote_lead; /* highest_remote - sim; healthy ≈ D */
    fprintf(stderr,
            "psxrecomp: rb runway sim=%u wire=%u remote_tip=%u D=%d P=%d "
            "pred_depth=%u remote_lead=%d runway_rem=%d cushion=%d "
            "rtt=%u rtt_raw=%u\n",
            (unsigned)sim, (unsigned)wire, (unsigned)highest_remote, D,
            g_np.input_prediction, (unsigned)pred_depth, remote_lead,
            runway_rem, g_cushion_rebuild, (unsigned)rtt, (unsigned)rtt_raw);
    fflush(stderr);
    (void)D;
}

static void np_admit_maybe_log_stats(uint32_t now)
{
    if (g_admit_stats_last_log_ms != 0u &&
        (uint32_t)(now - g_admit_stats_last_log_ms) < 5000u)
        return;
    g_admit_stats_last_log_ms = now ? now : 1u;
    fprintf(stderr,
            "psxrecomp: rb admit stats invent_gap1=%u gap2=%u gap3+=%u "
            "gap1_grace=%u gap1_case_a=%u gap1_case_b=%u tip_ema=%u "
            "invent_runway_empty=%u invent_tip_stale=%u "
            "invent_gap1_legacy=%u cushion_wait=%u "
            "pcap_stalls=%u pcap_enters=%u freeze=%d D=%d P=%d cushion=%d "
            "mispredict=%u note_late=%u suppressed_rb=%u suppressed_off=%u "
            "debt_ms=%u debt_added=%u\n",
            (unsigned)g_admit_invent_gap1, (unsigned)g_admit_invent_gap2,
            (unsigned)g_admit_invent_gap3p, (unsigned)g_admit_gap1_grace,
            (unsigned)g_admit_gap1_case_a, (unsigned)g_admit_gap1_case_b,
            (unsigned)g_tip_arrival_ema_ms,
            (unsigned)g_admit_invent_runway_empty,
            (unsigned)g_admit_invent_tip_stale,
            (unsigned)g_admit_invent_gap1_legacy,
            (unsigned)g_admit_cushion_wait,
            (unsigned)g_admit_pcap_stalls, (unsigned)g_admit_pcap_enters,
            g_pcap_frozen, g_np.input_delay, g_np.input_prediction,
            g_cushion_rebuild,
            (unsigned)g_ts_mispredict_count,
            (unsigned)g_ts_note_late_applied,
            (unsigned)g_ts_note_late_suppressed_rb,
            (unsigned)g_ts_note_late_suppressed_off,
            (unsigned)g_ts_debt_ms,
            (unsigned)g_ts_debt_added_ms);
    fflush(stderr);
}

static void np_adapt_delay_on_pcap_enter(uint32_t now)
{
    const char *e;

    if (g_adapt_delay_enabled < 0) {
        e = getenv("PSX_RB_ADAPT_DELAY");
        g_adapt_delay_enabled = (e && e[0]) ? (atoi(e) != 0) : 1;
    }
    if (!g_adapt_delay_enabled || !g_np.session)
        return;
    /* Host / sim-authority only — guests receive DELAY_SYNC. */
    if (g_np.local_slot != 0)
        return;

    if (g_pcap_window_t0_ms == 0u ||
        (uint32_t)(now - g_pcap_window_t0_ms) > RB_ADAPT_WINDOW_MS) {
        g_pcap_window_t0_ms = now ? now : 1u;
        g_pcap_freeze_enters_window = 0u;
    }
    g_pcap_freeze_enters_window++;

    if (g_pcap_freeze_enters_window < RB_ADAPT_FREEZE_ENTERS_THRESH)
        return;
    if (g_adapt_last_bump_ms != 0u &&
        (uint32_t)(now - g_adapt_last_bump_ms) < RB_ADAPT_COOLDOWN_MS)
        return;
    if (g_np.input_delay >= RB_ADAPT_DELAY_MAX)
        return;

    {
        int old_d = g_np.input_delay;
        int new_d = old_d + 1;
        if (new_d > RB_ADAPT_DELAY_MAX)
            new_d = RB_ADAPT_DELAY_MAX;
        if (rnet_session_request_delay_change(g_np.session, (rnet_u8)new_d)) {
            g_adapt_last_bump_ms = now ? now : 1u;
            g_pcap_freeze_enters_window = 0u;
            g_pcap_window_t0_ms = now ? now : 1u;
            fprintf(stderr,
                    "psxrecomp: rb adaptive delay bump %d → %d "
                    "(pcap freezes in window; P stays %d)\n",
                    old_d, new_d, g_np.input_prediction);
            fflush(stderr);
        }
    }
}

static void np_pcap_freeze_enter(rnet_u32 wire, rnet_u32 highest_remote, int pred)
{
    uint32_t now = np_mono_ms();
    g_admit_pcap_stalls++;
    if (!g_pcap_frozen) {
        g_pcap_frozen = 1;
        g_admit_pcap_enters++;
        fprintf(stderr,
                "psxrecomp: rb pcap FREEZE enter wire=%u remote=%u P=%d "
                "gap=%u D=%d\n",
                (unsigned)wire, (unsigned)highest_remote, pred,
                (unsigned)(wire > highest_remote ? wire - highest_remote : 0u),
                g_np.input_delay);
        fflush(stderr);
        np_adapt_delay_on_pcap_enter(now);
    }
    np_admit_maybe_log_stats(now);
}

static void np_pcap_freeze_exit(void)
{
    if (!g_pcap_frozen)
        return;
    g_pcap_frozen = 0;
    fprintf(stderr,
            "psxrecomp: rb pcap FREEZE exit (remote caught up / invent ok) "
            "D=%d enters=%u\n",
            g_np.input_delay, (unsigned)g_admit_pcap_enters);
    fflush(stderr);
}

/* Rollback admit: tip + invent remotes within P of remote tip; stall outside.
 * BattleShip phase_lock: invent only when wire_need <= highest_remote + P. */
static int np_try_admit_rollback(void)
{
    rnet_u32 sim = rnet_session_sim_tick(g_np.session);
    RNetInputSample sample;
    RNetRbFrame row;
    PsxNetPad pad;
    RNetSessionStats st;
    rnet_u8 delay_u8;
    rnet_u32 wire;
    int slot;
    int pred;
    int any_invent = 0;

    /* Tick FMV→settle tracker every admit (even when remotes are present). */
    (void)psx_netplay_rb_lockstep_no_invent();

    /* Mid-session DELAY_SYNC may have committed on the last advance. */
    np_sync_input_delay_from_session();

    if (!rnet_session_prepare_local_tip(g_np.session, sim))
        return 0;

    delay_u8 = (rnet_u8)(g_np.input_delay < 0 ? 0
                        : (g_np.input_delay > 255 ? 255 : g_np.input_delay));
    wire = rnet_wire_tick_from_sim(sim, delay_u8);
    pred = g_np.input_prediction;
    if (pred < 2) pred = 2;
    if (pred > 16) pred = 16;

    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    /* §28: feed tip-arrival cadence every admit tick (not just on miss) so
     * the gap1 Case A/B split has a real cadence to judge freshness by. */
    np_tip_track_advance(st.highest_remote_wire);

    /* TipHold past invent-cap: never invent (that caused tip-extend rereplay
     * cliffs). Advance only when every remote wire row is present *and* all
     * pads (local + remote) are idle (0xFFFF). Live-walking held digital
     * through the runway fed MotK menu key-repeat (~24 extra navigations per
     * press) and burned the coalesce window before tip-extend could absorb
     * the release (soak: tip-extend=0, tip-hold→commit adjacent). Missing
     * remotes or any held button → stall; wall-clock quiet / coalesce-ahead
     * owns the runway. */
    if (psx_netplay_rb_tip_holding()) {
        uint32_t tip = psx_netplay_rb_episode_target();
        uint32_t slack = psx_netplay_rb_tip_hold_invent_slack();
        if (tip > 0u && sim > tip + slack) {
            int missing = 0;
            int held = 0;
            for (slot = 0; slot < g_np.slot_count; ++slot) {
                if (slot == g_np.local_slot) {
                    if (rnet_session_peek_input(g_np.session, slot, wire,
                                                &sample)) {
                        decode_pad(&sample, &pad);
                        if (pad.buttons != 0xFFFFu)
                            held = 1;
                    } else if (g_np.staged_valid &&
                               g_np.staged.buttons != 0xFFFFu) {
                        held = 1;
                    }
                    continue;
                }
                if (!rnet_session_peek_remote_input(g_np.session, slot, wire,
                                                    &sample)) {
                    missing = 1;
                    break;
                }
                decode_pad(&sample, &pad);
                if (pad.buttons != 0xFFFFu)
                    held = 1;
            }
            if (missing || held)
                return 0;
        }
    }

    /* Phase alignment: the ahead peer paces down a few ms/tick so remote
     * rows arrive before the seal (kills invent-mispredict episodes at the
     * source). Never engages during episodes/lockstep — only live admits. */
    if (!psx_netplay_rb_active() &&
        np_timesync_throttle(wire))
        return 0;

    np_timesync_sample_lead(st.remote_lead);
    np_admit_log_runway(np_mono_ms(), sim, wire, st.highest_remote_wire,
                        st.remote_lead);
    np_phase_ctrl_maybe_log(np_mono_ms(), sim, st.remote_lead);

    /* Rebuild D cushion after episode: do not invent until remote tip is
     * nearly back at sim+D (remote_lead >= D-1). Both peers wait for real
     * inputs instead of racing the frontier with hold-last. */
    if (g_cushion_rebuild && !psx_netplay_rb_active()) {
        int need = (int)delay_u8 > 0 ? (int)delay_u8 - 1 : 0;
        if (st.remote_lead >= need) {
            g_cushion_rebuild = 0;
            fprintf(stderr,
                    "psxrecomp: rb cushion rebuilt remote_lead=%d D=%u\n",
                    st.remote_lead, (unsigned)delay_u8);
            fflush(stderr);
        }
    }

    for (slot = 0; slot < g_np.slot_count; ++slot) {
        if (slot == g_np.local_slot) {
            if (rnet_session_peek_input(g_np.session, slot, wire, &sample)) {
                decode_pad(&sample, &pad);
                netplay_ih_pad_to_frame(&pad, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            } else if (g_np.staged_valid) {
                netplay_ih_pad_to_frame(&g_np.staged, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            } else {
                memset(&pad, 0, sizeof(pad));
                pad.buttons = 0xFFFFu;
                pad.lx = pad.ly = pad.rx = pad.ry = 0x80u;
                pad.analog = 0; /* MotK digital default; never invent DualShock */
                pad.connected = 1;
                netplay_ih_pad_to_frame(&pad, sim, 0, &row);
                (void)netplay_ih_put(&g_np.ih, slot, &row);
            }
            continue;
        }

        if (rnet_session_peek_remote_input(g_np.session, slot, wire, &sample)) {
            decode_pad(&sample, &pad);
            netplay_ih_pad_to_frame(&pad, sim, 0, &row);
            (void)netplay_ih_put(&g_np.ih, slot, &row);
            /* Remote arrived — gap1 grace (if any) did its job. */
            np_gap1_note_grace_helped();
        } else {
            const char *invent_reason = NULL;
            static int s_gap1_legacy = -1; /* PSX_RB_GAP1_INVENT=1 → old path */
            static rnet_u32 s_miss_wire;
            static uint32_t s_miss_t0;
            uint32_t now_miss;
            uint32_t tip_stale_ms;
            rnet_u32 gap;

            /* FMV media + post-FMV lockstep: wait for remote wire (skip /
             * title Start). Invent idle opened tip episodes that hung. */
            if (psx_netplay_rb_lockstep_no_invent())
                return 0;
            /* Stall when invent would run more than P ahead of remote tip
             * (freeze + refill; adaptive delay may bump D on sustained
             * freeze enters — see §22). */
            if (wire > st.highest_remote_wire + (rnet_u32)pred) {
                np_pcap_freeze_enter(wire, st.highest_remote_wire, pred);
                return 0;
            }
            /* Cushion rebuild: wait for real remote rows (no invent). */
            if (g_cushion_rebuild && !psx_netplay_rb_active())
                return 0;

            if (s_gap1_legacy < 0) {
                const char *e = getenv("PSX_RB_GAP1_INVENT");
                /* §26: default ON — short gap1 grace then invent (rollback).
                 * Cushion-wait-until-TIP_STALE while remote_lead>0 was the
                 * WAN 30fps delay-sync path; ENV=0 restores that. */
                if (e && e[0])
                    s_gap1_legacy = (atoi(e) != 0) ? 1 : 0;
                else
                    s_gap1_legacy = 1;
                fprintf(stderr,
                        "psxrecomp: rb gap1 invent %s "
                        "(short grace then invent while remote_lead healthy%s)\n",
                        s_gap1_legacy ? "ON" : "OFF",
                        s_gap1_legacy ? "" : "; PSX_RB_GAP1_INVENT=0");
                fflush(stderr);
            }

            gap = (wire > st.highest_remote_wire)
                      ? (wire - st.highest_remote_wire)
                      : 0u;
            now_miss = np_mono_ms();
            if (s_miss_wire != wire) {
                s_miss_wire = wire;
                s_miss_t0 = now_miss;
            }

            /* Default: invent is last resort. While remote_lead > 0 the
             * confirmed tip is still ahead of sim — keep consuming wait,
             * not prediction. Ideal LAN: lead≈D, pred_depth=0, never invent.
             * Safety: if tip stalls too long, invent as TIP_STALE. */
            if (!s_gap1_legacy && st.remote_lead > 0) {
                tip_stale_ms = np_invent_rtt_ms(NULL) * 4u;
                if (tip_stale_ms < 150u)
                    tip_stale_ms = 150u;
                if (tip_stale_ms > 400u)
                    tip_stale_ms = 400u;
                if ((uint32_t)(now_miss - s_miss_t0) < tip_stale_ms) {
                    static rnet_u32 s_cw_wire;
                    if (s_cw_wire != wire) {
                        s_cw_wire = wire;
                        g_admit_cushion_wait++;
                    }
                    /* gap=1: still count as grace-wait for telemetry. */
                    if (gap == 1u) {
                        static rnet_u32 s_gap1_counted_wire;
                        if (s_gap1_counted_wire != wire) {
                            s_gap1_counted_wire = wire;
                            g_admit_gap1_grace++;
                            np_admit_maybe_log_stats(now_miss);
                        }
                    }
                    return 0;
                }
                invent_reason = "TIP_STALE";
                g_admit_invent_tip_stale++;
            } else if (s_gap1_legacy && gap == 1u) {
                /* §29: §28's per-miss Case A wait was reverted — soak data
                 * showed remote_lead sitting at a rock-stable D-1 the whole
                 * match (not drifting, not recovering), the signature of a
                 * fixed ~1-tick network transit delay, not a wait-it-out
                 * jitter blip. Waiting 4-10ms on every such miss only added
                 * admit tax (88% still expired into invent) and inflated
                 * `tip_ema` in a feedback loop (soak: 15ms -> 30-40ms),
                 * making later misses look "fresher" than they were. A
                 * fixed transit delay cannot be waited out per-tick; it can
                 * only be closed by pacing the peer that is structurally
                 * racing ahead (see np_timesync_note_late — that mechanism
                 * IS asymmetric-safe, per soak evidence one side owns
                 * essentially all mispredicts) or absorbed with more delay
                 * (D). So gap=1 invents immediately again (back to §27),
                 * classified for diagnostics only (GAP1_PHASE = tip was
                 * healthy/advancing when we inverted; GAP1_LEGACY = it was
                 * not) at zero added latency. PSX_RB_GAP1_GRACE_MS still
                 * forces the old flat-cap wait for A/B testing. */
                int has_override = 0;
                uint32_t gap1_cap = np_gap1_grace_cap_ms(&has_override);
                int case_a = 0;

                if (!has_override) {
                    uint32_t tip_age = np_tip_age_ms();
                    uint32_t period =
                        g_tip_arrival_ema_ms ? g_tip_arrival_ema_ms : 17u;

                    case_a = (st.remote_lead >= 1) &&
                             (tip_age != 0xffffffffu) &&
                             (tip_age * 2u < period * RB_TIP_FRESH_MULT_X2);
                    if (case_a)
                        g_admit_gap1_case_a++;
                    else
                        g_admit_gap1_case_b++;
                    gap1_cap = 0u; /* no wait — classification only */
                }
                if (gap1_cap != 0u) {
                    if (np_invent_grace_stall_ex(slot, wire, gap1_cap, 0)) {
                        static rnet_u32 s_gap1_counted_wire;
                        if (s_gap1_counted_wire != wire) {
                            s_gap1_counted_wire = wire;
                            g_admit_gap1_grace++;
                            np_admit_maybe_log_stats(np_mono_ms());
                        }
                        return 0;
                    }
                    np_gap1_note_expire_invent();
                }
                invent_reason = case_a ? "GAP1_PHASE" : "GAP1_LEGACY";
                g_admit_invent_gap1_legacy++;
            } else {
                /* §27 shallow invent: pred_depth≥2 only after tip looks stale
                 * (1× invent RTT, floor 40ms). Avoids burning deep into P
                 * every tick (constant deep resim). Then short grace. */
                uint32_t pred_depth = gap;
                tip_stale_ms = np_invent_rtt_ms(NULL);
                if (tip_stale_ms < RB_INVENT_DEPTH_STALE_FLOOR_MS)
                    tip_stale_ms = RB_INVENT_DEPTH_STALE_FLOOR_MS;
                if (pred_depth >= 2u &&
                    (uint32_t)(now_miss - s_miss_t0) < tip_stale_ms) {
                    return 0;
                }
                if (np_invent_grace_stall_ex(slot, wire,
                                             RB_INVENT_RUNWAY_GRACE_CAP_MS, 1))
                    return 0;
                invent_reason = "RUNWAY_EMPTY";
                g_admit_invent_runway_empty++;
            }

            /* MotK digital: hold-last. Idle invent re-mismatched every held
             * D-pad tick after commit → episode storm / char-select freeze.
             * Menu release soft-promote is off (see reconcile) so sticky Up
             * cannot skip a needed resim. Seal gap-fill stays idle. */
            np_admit_note_invent_gap(wire, st.highest_remote_wire);
            np_admit_log_invent(sim, wire, st.highest_remote_wire,
                                st.remote_lead, invent_reason);
            any_invent = 1;
            (void)netplay_ih_invent_hold_last(&g_np.ih, slot, sim, &row);
        }
    }

    /* Remote caught up or we invented inside P — leave freeze if armed. */
    np_pcap_freeze_exit();
    if (any_invent)
        np_admit_maybe_log_stats(np_mono_ms());

    np_publish_hist_sio(sim);
    g_np.needs_advance = 1;
    return 1;
}

int psx_netplay_active(void)
{
    return g_np.active && g_np.session != NULL;
}

int psx_netplay_is_running(void)
{
    return psx_netplay_active() && rnet_session_is_running(g_np.session);
}

const char *psx_netplay_transport_name(void)
{
    if (!psx_netplay_active()) return "none";
    return g_np.use_ice ? "ice" : "lan";
}

int psx_netplay_ice_failed(void)
{
#if defined(RNET_ENABLE_ICE)
    if (!psx_netplay_active() || !g_np.use_ice)
        return 0;
    return rnet_session_ice_state(g_np.session) == RNET_ICE_STATE_FAILED;
#else
    return 0;
#endif
}

int psx_netplay_local_slot(void)
{
    return psx_netplay_active() ? g_np.local_slot : -1;
}

int psx_netplay_input_player(void)
{
    return psx_netplay_active() ? g_np.input_player : 0;
}

uint32_t psx_netplay_sim_tick(void)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_sim_tick(g_np.session);
}

/* §34 MotK digital: bridge 1–2 tick idle holes so controller bounce does not
 * publish press/idle/press edges (menu edge-triggers → double navigations).
 * Longer gaps stay real releases; tip-hold idle-dwell covers the rest. */
#define NP_DIGITAL_RELEASE_DEBOUNCE_TICKS 2u
static uint16_t g_dig_sticky_buttons = 0xFFFFu;
static uint32_t g_dig_sticky_idle_n;

static void np_digital_debounce_staged(void)
{
    uint16_t b;
    if (!g_np.rollback || !g_np.staged_valid)
        return;
    b = g_np.staged.buttons;
    if (b == 0xFFFFu && g_dig_sticky_buttons != 0xFFFFu) {
        if (g_dig_sticky_idle_n < NP_DIGITAL_RELEASE_DEBOUNCE_TICKS) {
            g_np.staged.buttons = g_dig_sticky_buttons;
            g_dig_sticky_idle_n++;
        } else {
            g_dig_sticky_buttons = 0xFFFFu;
            g_dig_sticky_idle_n = 0u;
        }
    } else if (b != 0xFFFFu) {
        g_dig_sticky_buttons = b;
        g_dig_sticky_idle_n = 0u;
    }
}

void psx_netplay_stage_local(const PsxNetPad *pad)
{
    if (!pad) {
        g_np.staged_valid = 0;
        g_np.live_valid = 0;
        return;
    }
    /* Always refresh live physical snapshot first. TipHold invent-cap parks
     * sim (and therefore the latched staged sample); SAFETY/quiet must still
     * observe a real release (§36). */
    g_np.live = *pad;
    psx_netplay_normalize_pad(&g_np.live);
    g_np.live_valid = 1;
    /* Once running, freeze the first sample for the current sim tick so
     * re-admits / barrier retries cannot change the INPUT_CONFIRM hash. */
    if (psx_netplay_active() && rnet_session_is_running(g_np.session)) {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        if (g_np.latched_for_tick && g_np.latched_sim_tick == t)
            return;
        g_np.staged = *pad;
        psx_netplay_normalize_pad(&g_np.staged);
        np_digital_debounce_staged();
        g_np.staged_valid = 1;
        g_np.latched_for_tick = 1;
        g_np.latched_sim_tick = t;
        return;
    }
    /* Linking: keep refreshing released/local pads until START. */
    g_np.staged = *pad;
    psx_netplay_normalize_pad(&g_np.staged);
    g_np.staged_valid = 1;
}

int psx_netplay_needs_local_sample(void)
{
    if (!psx_netplay_active()) return 0;
    if (!rnet_session_is_running(g_np.session)) return 1; /* linking */
    {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        return !(g_np.latched_for_tick && g_np.latched_sim_tick == t);
    }
}

int psx_netplay_live_pad_buttons(uint16_t *out)
{
    if (!out)
        return 0;
    if (!g_np.live_valid) {
        *out = 0xFFFFu;
        return 0;
    }
    *out = g_np.live.buttons;
    return 1;
}

int psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    if (!psx_netplay_active()) return 0;
    return rnet_session_input_desync(g_np.session, tick, local_hash, remote_hash);
}

int psx_netplay_peer_disconnected(uint32_t timeout_ms)
{
    if (!psx_netplay_active()) return 0;
    /* timeout_ms == 0: BYE / peer_gone only (no silence timeout). Used during
     * load barriers where INPUT is suppressed for seconds. */
    return rnet_session_peer_disconnected(g_np.session, (rnet_u64)timeout_ms);
}

static void np_diag_capture(const PsxNetplayConfig *cfg, int slots)
{
    const char *arch = "p2p";
    int players;
    if (!cfg) return;
    if (cfg->force_input_relay)
        arch = "server_relay";
    else if (slots >= 3)
        arch = "host_relay";
    players = cfg->player_count > 0 ? cfg->player_count : slots;
    if (players < 1) players = slots;
    snprintf(g_np_diag_arch, sizeof(g_np_diag_arch), "%s", arch);
    g_np_diag_max_players = slots;
    g_np_diag_player_count = players;
    g_np_diag_configured = 1;
}

#if defined(__linux__)
static int peer_is_loopback(const char *peer_hostport)
{
    if (!peer_hostport || !peer_hostport[0]) return 0;
    if (strncmp(peer_hostport, "127.", 4) == 0) return 1;
    if (strncmp(peer_hostport, "localhost:", 10) == 0) return 1;
    if (strncmp(peer_hostport, "::1:", 4) == 0) return 1;
    if (strcmp(peer_hostport, "::1") == 0) return 1;
    return 0;
}

/* Same-machine MotK FMV: lockstep syncs both peers' MDEC peaks; pinning each
 * slot to a disjoint CPU half cut headless FMV ~40 → ~45 in A/B. */
static void pin_localhost_peer_cpus(int local_slot)
{
    long ncpu;
    cpu_set_t set;
    int i, lo, hi;

    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 4) return;
    CPU_ZERO(&set);
    if (local_slot <= 0) {
        lo = 0;
        hi = (int)(ncpu / 2);
    } else {
        lo = (int)(ncpu / 2);
        hi = (int)ncpu;
    }
    for (i = lo; i < hi; i++)
        CPU_SET(i, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
}
#endif

#if defined(PSX_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
static void host_on_signal(const RNetSignal *msg, void *ctx)
{
    (void)ctx;
    if (!msg) return;
    (void)psx_lobby_send_signal((int)msg->type, (int)msg->flag, msg->text);
}

static void drain_lobby_signals(void)
{
    int type = 0, flag = 0;
    char text[2048];
    if (!g_np.session) return;
    while (psx_lobby_poll_signal(&type, &flag, text, sizeof(text))) {
        RNetSignal sig;
        memset(&sig, 0, sizeof(sig));
        /* Peers emit LOCAL_*; push_signal expects REMOTE_* for SDP/candidates. */
        if (type == (int)RNET_SIGNAL_LOCAL_SDP)
            type = (int)RNET_SIGNAL_REMOTE_SDP;
        else if (type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
            type = (int)RNET_SIGNAL_REMOTE_CANDIDATE;
        sig.type = (RNetSignalType)type;
        sig.flag = (rnet_u8)(flag & 0xFF);
        strncpy(sig.text, text, sizeof(sig.text) - 1);
        rnet_session_push_signal(g_np.session, &sig);
    }
}
#else
static void drain_lobby_signals(void) {}
#endif

static int resolve_use_ice(const PsxNetplayConfig *cfg)
{
    int in_motk_room = 0;

    if (cfg->transport == 2) return 0; /* force LAN */
#if defined(PSX_HAS_LOBBY_CLIENT)
    in_motk_room = psx_lobby_connected() && psx_lobby_in_lobby();
#endif

    /* Server UDP pad relay: dial relay_endpoint with LAN transport (not ICE).
     * MotK previously always preferred ICE and ignored the relay rewrite. */
    if (cfg->force_input_relay) {
        if (!cfg->peer_hostport || !cfg->peer_hostport[0]) {
            fprintf(stderr,
                    "psx_netplay: force_input_relay set but peer/relay "
                    "endpoint empty\n");
            return -1;
        }
        fprintf(stderr,
                "psx_netplay: server input relay — LAN transport to %s\n",
                cfg->peer_hostport);
        return 0;
    }

#if defined(RNET_ENABLE_ICE) && defined(PSX_HAS_LOBBY_CLIENT)
    if (cfg->transport == 1) {
        if (!in_motk_room) {
            fprintf(stderr,
                    "psx_netplay: ICE requested but MotK lobby not connected\n");
            return -1;
        }
        return 1;
    }
    /* Auto: hosted MotK room always uses ICE. Do not demote to LAN when the
     * lobby rewrites 0.0.0.0 binds to a private TCP peer IP (often wrong).
     * Direct IP / LAN file lobby (no MotK seat) stays on LAN UDP. */
    if (in_motk_room)
        return 1;
    return 0;
#else
    {
        int online_requested = cfg->transport == 1 ||
                               (cfg->transport == 0 && in_motk_room);
        if (online_requested) {
            fprintf(stderr,
                    "psx_netplay: hosted lobby requires ICE, but ICE is not "
                    "available in this build (configure with PSX_NET_ICE=ON / "
                    "RNET_ENABLE_ICE=ON)\n");
            return -1;
        }
    }
    return 0;
#endif
}


int psx_netplay_start(const PsxNetplayConfig *cfg)
{
    RNetConfig rcfg;
    RNetHostVTable host;
    int in_player;
    int slots;
    int local;
    int use_ice;

    if (!cfg || !cfg->enabled) return -1;
    if (g_np.session) psx_netplay_shutdown();

    slots = cfg->slot_count;
    if (slots < 2) slots = 2;
    if (slots > PSX_MAX_PLAYERS) slots = PSX_MAX_PLAYERS;
    if (slots > RNET_MAX_SLOTS) slots = RNET_MAX_SLOTS;

    local = cfg->local_slot;
    if (local < 0) local = 0;
    if (local >= slots) local = slots - 1;

    rnet_config_init_defaults(&rcfg);
    rcfg.slot_count = (rnet_u8)slots;
    rcfg.local_slot = (rnet_u8)local;
    /* Max delay 20 matches RNET_MAX_BUNDLE 21 (neutral prefix + tip). */
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0
                               : (cfg->input_delay > 20 ? 20 : cfg->input_delay));
    rcfg.session_id = cfg->session_id ? cfg->session_id : 1u;

    /* Host resolves auto (-1) before start; accept 0..PSX_MAX_PLAYERS-1. */
    in_player = cfg->input_player;
    if (in_player < 0 || in_player >= PSX_MAX_PLAYERS) in_player = 0;

    use_ice = resolve_use_ice(cfg);
    if (use_ice < 0)
        return -4;

    memset(&host, 0, sizeof(host));
    host.sample_local = host_sample_local;
    host.publish = host_publish;
    host.ctx = &g_np;
#if defined(PSX_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
    if (use_ice)
        host.on_signal = host_on_signal;
#endif

    g_np.session = rnet_session_create(&rcfg, &host);
    if (!g_np.session) return -2;

    if (use_ice) {
#if defined(RNET_ENABLE_ICE)
        RNetIceConfig ice;
        RNetIpv4Address addrs[8];
        int naddr;
        const char *env_turn_host = getenv("PSX_NET_TURN_HOST");
        const char *env_turn_user = getenv("PSX_NET_TURN_USER");
        const char *env_turn_pass = getenv("PSX_NET_TURN_PASS");
        const char *env_stun = getenv("PSX_NET_STUN_HOST");

        g_np.ice_has_turn = 0;
        g_np.ice_stun_host[0] = '\0';
        g_np.ice_turn_host[0] = '\0';
        g_np.ice_turn_user[0] = '\0';
        g_np.ice_turn_pass[0] = '\0';
        g_np.ice_bind_addr[0] = '\0';

        rnet_ice_config_init_defaults(&ice);
        ice.controlling = (rcfg.local_slot == 0) ? 1u : 0u;

        naddr = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
        if (naddr > 0 && addrs[0].address[0]) {
            snprintf(g_np.ice_bind_addr, sizeof(g_np.ice_bind_addr), "%s",
                     addrs[0].address);
            ice.bind_address = g_np.ice_bind_addr;
        }

#if defined(PSX_HAS_LOBBY_CLIENT)
        /* Prefer TURN prefetched at WS welcome; re-request and wait if stale. */
        if (psx_lobby_connected()) {
            int i;
            const PsxLobbyTurnCredentials *tc = psx_lobby_turn_credentials();
            if (!tc || !tc->valid) {
                (void)psx_lobby_request_turn_credentials();
                for (i = 0; i < 200; ++i) { /* up to ~2s */
                    tc = psx_lobby_turn_credentials();
                    if (tc && tc->valid)
                        break;
                    psx_lobby_pump();
                    np_sleep_ms(10);
                }
            }
        }
        {
            const PsxLobbyTurnCredentials *tc = psx_lobby_turn_credentials();
            if (tc && tc->valid) {
                if (tc->stun_host[0]) {
                    snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host),
                             "%s", tc->stun_host);
                    ice.stun_host = g_np.ice_stun_host;
                    ice.stun_port = (rnet_u16)(tc->stun_port > 0 ? tc->stun_port
                                                                  : 3478);
                }
                snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                         tc->turn_host);
                snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                         tc->username);
                snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                         tc->password);
                ice.turn_host = g_np.ice_turn_host;
                ice.turn_user = g_np.ice_turn_user;
                ice.turn_pass = g_np.ice_turn_pass;
                ice.turn_port = (rnet_u16)(tc->turn_port > 0 ? tc->turn_port
                                                              : 3478);
                g_np.ice_has_turn = 1;
            }
        }
#endif
        if (env_stun && env_stun[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     env_stun);
            ice.stun_host = g_np.ice_stun_host;
            ice.stun_port = (rnet_u16)env_u("PSX_NET_STUN_PORT", ice.stun_port
                                                                     ? ice.stun_port
                                                                     : 3478);
        }
        if (env_turn_host && env_turn_host[0] && env_turn_user &&
            env_turn_user[0] && env_turn_pass && env_turn_pass[0]) {
            snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                     env_turn_host);
            snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                     env_turn_user);
            snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                     env_turn_pass);
            ice.turn_host = g_np.ice_turn_host;
            ice.turn_user = g_np.ice_turn_user;
            ice.turn_pass = g_np.ice_turn_pass;
            ice.turn_port = (rnet_u16)env_u("PSX_NET_TURN_PORT", 3478);
            g_np.ice_has_turn = 1;
        }

        if (!g_np.ice_stun_host[0] && ice.stun_host && ice.stun_host[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     ice.stun_host);
        }
        g_np.ice_stun_port = ice.stun_port ? (unsigned)ice.stun_port : 19302u;
        g_np.ice_turn_port = ice.turn_port ? (unsigned)ice.turn_port : 0u;

        if (g_np.ice_has_turn) {
            fprintf(stderr,
                    "psx_netplay: ICE stun=%s:%u turn=%s:%u user=%s bind=%s\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.turn_host, (unsigned)ice.turn_port, ice.turn_user,
                    ice.bind_address ? ice.bind_address : "(any)");
        } else {
            const char *allow_stun = getenv("PSX_NET_ALLOW_STUN_ONLY");
            fprintf(stderr,
                    "psx_netplay: ICE STUN-only (no TURN) stun=%s:%u "
                    "bind=%s — online MotK requires Coturn "
                    "(lobby get_turn_credentials or PSX_NET_TURN_*); set "
                    "PSX_NET_ALLOW_STUN_ONLY=1 to override\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.bind_address ? ice.bind_address : "(any)");
            /* BattleShip-style: refuse WAN ICE without TURN (CGNAT hangs). */
            if (!allow_stun || !allow_stun[0] || allow_stun[0] == '0') {
                rnet_session_destroy(g_np.session);
                g_np.session = NULL;
                return -4;
            }
        }

        {
            /* Online default is Force TURN (match_caps / UI); env overrides. */
            int force_turn = cfg->force_turn ? 1 : 0;
            const char *ft = getenv("PSX_NET_FORCE_TURN");
            if (ft && ft[0] && ft[0] != '0')
                force_turn = 1;
            else if (ft && ft[0] == '0')
                force_turn = 0;
            if (force_turn && !g_np.ice_has_turn) {
                fprintf(stderr,
                        "psx_netplay: FORCE_TURN requires Coturn credentials "
                        "(lobby get_turn_credentials or PSX_NET_TURN_*)\n");
                rnet_session_destroy(g_np.session);
                g_np.session = NULL;
                return -4;
            }
            if (force_turn) {
                ice.force_relay = 1;
                fprintf(stderr,
                        "psx_netplay: FORCE_TURN — ICE will use relay-only "
                        "candidates (host match_caps / all peers)\n");
            }
        }

        if (rnet_session_start_ice(g_np.session, &ice) != 0) {
            fprintf(stderr,
                    "psx_netplay: start_ice failed; refusing unsafe LAN "
                    "fallback for an online lobby\n");
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -4;
        }
#else
        fprintf(stderr, "psx_netplay: ICE requested but not built\n");
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
        return -4;
#endif
    }

    if (!use_ice) {
        /* Host-as-relay: slot 0 with 3+ seats and no dial peer. */
#if PSX_MAX_PLAYERS >= 3
        const int peer_empty =
            !cfg->peer_hostport || !cfg->peer_hostport[0];
        const int use_hub = (local == 0 && slots >= 3 && peer_empty);
        const int rc = use_hub
            ? rnet_session_start_lan_hub(g_np.session, cfg->bind_hostport)
            : rnet_session_start_lan(g_np.session, cfg->bind_hostport,
                                    cfg->peer_hostport);
#else
        const int rc = rnet_session_start_lan(g_np.session, cfg->bind_hostport,
                                              cfg->peer_hostport);
#endif
        if (rc != 0) {
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -3;
        }
    }
    np_diag_capture(cfg, slots);
    g_np.active = 1;
    /* Before any snap/resim: SW GPU so VRAM is bit-identical across peers. */
    psx_frontend_netplay_force_sw_gpu();
    netplay_hc_reset(&g_np.hc);
    np_part_ring_reset();
    g_np.rollback = cfg->rollback ? 1 : 0;
    netplay_ih_reset(&g_np.ih, (int)rcfg.slot_count);
    g_np.pending_rewind = 0;
    g_np.pending_rewind_tick = 0;
    g_np.pending_rewind_slot = 0;
    /* Seat / delay / integrity MUST be live before rb_start — RNetRbSession
     * freezes local_slot + slot_count at create. Starting with zeroed g_np made
     * every peer seal as slot 0 and export the wrong seat (VS-select hang). */
    g_np.use_ice = use_ice ? 1 : 0;
    g_np.slot_count = (int)rcfg.slot_count;
    g_np_slot_count = g_np.slot_count;
    g_np.local_slot = (int)rcfg.local_slot;
    g_np.input_player = in_player;
    g_np.input_delay = (int)rcfg.input_delay;
    g_np.input_prediction = cfg->input_prediction;
    if (g_np.input_prediction < 2) g_np.input_prediction = 2;
    if (g_np.input_prediction > 16) g_np.input_prediction = 16;
    {
        uint32_t bios = 0, entry = 0;
        savestate_get_integrity(&bios, &entry);
        g_np.bios_checksum = bios;
        g_np.entry_pc = entry;
    }
    if (g_np.rollback) {
        np_rb_bind_and_start();
        printf("psxrecomp: netplay mode=rollback (D=%d P=%d invent+contract)\n",
               g_np.input_delay, g_np.input_prediction);
        /* Peers MUST share one binary — mixed build-release vs packaged
         * motk-* left matched digests for hundreds of ticks then GPR/tim
         * forks in Replay, with pin zlib ~1.34M vs ~1.13M. */
        {
            char exe[512];
            long long sz = -1;
#if defined(_WIN32)
            DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
            if (n == 0 || n >= sizeof(exe))
                snprintf(exe, sizeof(exe), "(unknown)");
            {
                WIN32_FILE_ATTRIBUTE_DATA fad;
                if (GetFileAttributesExA(exe, GetFileExInfoStandard, &fad)) {
                    ULARGE_INTEGER u;
                    u.HighPart = fad.nFileSizeHigh;
                    u.LowPart = fad.nFileSizeLow;
                    sz = (long long)u.QuadPart;
                }
            }
#else
            ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            if (n < 0) {
                snprintf(exe, sizeof(exe), "(unknown)");
            } else {
                exe[n] = '\0';
                {
                    struct stat st;
                    if (stat(exe, &st) == 0)
                        sz = (long long)st.st_size;
                }
            }
#endif
            fprintf(stderr,
                    "psxrecomp: rb binary path=%s size=%lld "
                    "(peers must match bit-identical)\n",
                    exe, sz);
            fflush(stderr);
        }
        fflush(stdout);
    }
    if (g_np.slot_count >= 3)
        sio_set_multitap(1);
    else
        sio_set_multitap(0);
    g_np.staged_valid = 0;
    g_np.live_valid = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.latched_sim_tick = 0;
    g_np.xfer = NP_XFER_NONE;
    g_np.xfer_slot = 0;
    g_np.mc_sync_done = 0;
    g_np.mc_sync_sent = 0;
    g_np.local_save_staged = 0;
    g_np.load_applied_local = 0;
    g_np.guest_sandbox = 0;
    g_np.force_input_relay = cfg->force_input_relay ? 1 : 0;
    g_np.session_id = rcfg.session_id;
    g_np.is_host = (g_np.local_slot == 0) ? 1 : 0;
    g_np.frames_finished = 0;
    g_np.diag_session++;
    g_diag_summary_written = 0;
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_last_write_ms = 0;
    snprintf(g_np.bind_hostport, sizeof(g_np.bind_hostport), "%s",
             cfg->bind_hostport);
    snprintf(g_np.peer_hostport, sizeof(g_np.peer_hostport), "%s",
             cfg->peer_hostport);
    g_np.lobby_server[0] = '\0';
    g_np.lobby_id[0] = '\0';
#if defined(PSX_HAS_LOBBY_CLIENT)
    if (use_ice && psx_lobby_connected() && psx_lobby_in_lobby()) {
        const PsxLobbyJoinInfo *ji = psx_lobby_join_info();
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "hosted_lobby");
        snprintf(g_np.lobby_server, sizeof(g_np.lobby_server), "%s",
                 psx_lobby_default_url());
        if (ji && ji->lobby_id[0])
            snprintf(g_np.lobby_id, sizeof(g_np.lobby_id), "%s", ji->lobby_id);
        g_np.is_host = psx_lobby_is_host() ? 1 : 0;
    } else
#endif
    {
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "direct_ip");
    }

#if defined(__linux__)
    if (!use_ice && peer_is_loopback(cfg->peer_hostport))
        pin_localhost_peer_cpus(g_np.local_slot);
#endif

    psx_netplay_release_pads();
    fprintf(stderr,
            "psx_netplay: started transport=%s slot=%d input_player=%d session=%u "
            "delay=%u force_input_relay=%d force_turn=%d bind=%s peer=%s\n",
            use_ice ? "ice" : "lan", g_np.local_slot, g_np.input_player,
            (unsigned)rcfg.session_id, (unsigned)rcfg.input_delay,
            g_np.force_input_relay, cfg->force_turn ? 1 : 0, cfg->bind_hostport,
            use_ice ? "(ice)" : cfg->peer_hostport);
    return 0;
}

void psx_netplay_bind_guest_saves(void)
{
    if (!psx_netplay_active() || g_np.local_slot == 0 || g_np.guest_sandbox)
        return;
    np_enter_guest_sandbox();
}

/* Delay-sync starvation hold (lockstep-safe; mirrors snes_host_barrier_admit). */
#define PSX_STARVATION_ENTER_DEFAULT 4
#define PSX_STARVATION_EXIT_DEFAULT 3
#define PSX_STARVATION_EXIT_HR_LEAD_DEFAULT 0
#define PSX_STARVATION_GRACE_TICKS 60
/* Default 0: after starvation clears, resume ~1 sim/wall frame and let
 * remote_lead rebuild toward D instead of a turbo recovery burst.
 * Override: PSX_NET_STARVATION_RECOVERY_BURST / PSX_NET_CATCHUP_CAP. */
#define PSX_STARVATION_RECOVERY_BURST_DEFAULT 0
#define PSX_CATCHUP_CAP_DEFAULT 0

static struct {
    int latched;
    int enter_run;
    int exit_run;
    int recovery_amount;
    int latch_logged;
    int just_cleared;
} g_starv;

/* Defined below poll_admit; used by the starvation runway check. */
int psx_netplay_remote_lead(void);
int psx_netplay_input_delay(void);

static int np_starv_env_int(const char *name, int def)
{
    const char *v = getenv(name);
    long n;
    char *end;
    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 0 || n > 64)
        return def;
    return (int)n;
}

static void np_starv_reset(void)
{
    memset(&g_starv, 0, sizeof(g_starv));
}

static int np_starv_runway_ok(void)
{
    int lead = psx_netplay_remote_lead();
    int delay = psx_netplay_input_delay();
    int hr_lead = np_starv_env_int("PSX_NET_STARVATION_EXIT_HR_LEAD",
                                   PSX_STARVATION_EXIT_HR_LEAD_DEFAULT);
    if (delay < 0)
        delay = 0;
    return lead >= delay + hr_lead;
}

void psx_netplay_shutdown(void)
{
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_summary_written = 0;
    g_diag_last_write_ms = 0;
    if (g_np.session) {
        (void)rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
    }
    psx_netplay_rb_shutdown();
    psx_netplay_rb_bind(NULL);
    np_leave_guest_sandbox();
    {
        CPUState *saved_cpu = g_np.cpu;
        memset(&g_np, 0, sizeof(g_np));
        g_np.cpu = saved_cpu;
        netplay_hc_reset(&g_np.hc);
        np_part_ring_reset();
    }
    np_starv_reset();
}

int psx_netplay_is_host(void)
{
    return psx_netplay_active() && g_np.local_slot == 0;
}

int psx_netplay_request_save(int slot)
{
    uint32_t sim;
    uint32_t delay;
    uint32_t target;
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1; /* guest: host-only; ignore */
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;

    /* Agree a future sim_tick so TURN/coord latency cannot make the host
     * write tick T while the guest still writes T+k (CRC miss → transfer).
     * crc field of size==0 probe carries the target tick. */
    sim = rnet_session_sim_tick(g_np.session);
    delay = (uint32_t)psx_netplay_input_delay();
    if (delay < 1u) delay = 1u;
    target = sim + delay + 2u;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)slot, 0,
                                 target) != 0)
        return 1;
    g_np.xfer = NP_XFER_SAVE_COORD;
    g_np.xfer_slot = slot;
    g_np.save_target_tick = target;
    g_np.local_save_staged = 0;
    g_np.local_save_acked = 0;
    printf("psxrecomp: netplay save slot=%d — coordinating local writes "
           "(target sim=%u, now=%u)…\n",
           slot, (unsigned)target, (unsigned)sim);
    fflush(stdout);
    return 1;
}

int psx_netplay_request_load(int slot)
{
    uint32_t size = 0, crc = 0;
    char reason[192];
    if (!psx_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0)
        return 1;
    if (np_xfer_busy() || !g_np.mc_sync_done)
        return 1;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;
    if (!savestate_slot_compatible(slot, reason, sizeof(reason))) {
        printf("psxrecomp: netplay load slot=%d refused — %s "
               "(resave with this build: Shift+F%d)\n",
               slot, reason[0] ? reason : "incompatible", slot + 1);
        fflush(stdout);
        return 1;
    }
    if (!np_slot_crc(slot, &size, &crc))
        return 1;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)slot, size, crc) != 0)
        return 1;
    g_np.xfer = NP_XFER_LOAD_PROBE;
    g_np.xfer_slot = slot;
    g_np.load_applied_local = 0;
    g_np.load_apply_failed = 0;
    printf("psxrecomp: netplay load slot=%d — hash probe (%u bytes)\n", slot, (unsigned)size);
    fflush(stdout);
    return 1;
}

int psx_netplay_in_load_barrier(void)
{
    if (!psx_netplay_active())
        return 0;
    /* Any save/load/memcard sync phase — TURN chunk xfers of ~1.4MB need the
     * 90s budget (20s admit stall was killing SAVE mid-transfer). */
    return (g_np.xfer != NP_XFER_NONE) ? 1 : 0;
}

int psx_netplay_consume_load_apply_failed(void)
{
    int v = g_np.load_apply_failed;
    g_np.load_apply_failed = 0;
    return v;
}


static int np_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PSX_NET_DIAG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* Verbose delay-sync starvation latch/clear spam. Off by default — the latch
 * can toggle every few frames under jitter and floods stderr. Enable with
 * PSX_NET_DELAY_SYNC_DIAG=1 (alias: PSX_NET_STARVATION_DIAG=1). */
static int np_delay_sync_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PSX_NET_DELAY_SYNC_DIAG");
        if (!v || !v[0])
            v = getenv("PSX_NET_STARVATION_DIAG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

static unsigned np_diag_interval_ms(void)
{
    static unsigned cached = 0;
    unsigned hz;
    if (cached)
        return cached;
    hz = env_u("PSX_NET_DIAG_HZ", 2);
    if (hz < 1) hz = 1;
    if (hz > 30) hz = 30;
    cached = 1000u / hz;
    if (cached < 1) cached = 1;
    return cached;
}

static void np_diag_escape(char *out, size_t out_len, const char *in)
{
    size_t oi = 0;
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;
    for (; *in && oi + 2 < out_len; ++in) {
        char c = *in;
        if (c == '"' || c == '\\') {
            if (oi + 3 >= out_len)
                break;
            out[oi++] = '\\';
            out[oi++] = c;
        } else if ((unsigned char)c < 0x20) {
            /* skip */
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = '\0';
}

static const char *np_diag_ice_path(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return "lan";
    if (!st)
        return "pending";
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return "failed";
    if (st->ice_path[0])
        return st->ice_path;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return "unknown";
    return "pending";
}

static const char *np_diag_ice_nat(const char *path)
{
    if (!g_np.use_ice)
        return "lan";
    if (!path || !path[0] || strcmp(path, "pending") == 0)
        return "pending";
    if (strcmp(path, "failed") == 0)
        return "failed";
    if (strcmp(path, "relay") == 0)
        return "turn";
    if (strcmp(path, "srflx") == 0 || strcmp(path, "prflx") == 0)
        return "stun";
    if (strcmp(path, "host") == 0)
        return "host";
    return "unknown";
}

static int np_diag_path_ready(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return 1;
    if (!st)
        return 0;
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return 1;
    if (st->ice_path[0] && strcmp(st->ice_path, "pending") != 0 &&
        strcmp(st->ice_path, "unknown") != 0)
        return 1;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return 1;
    return 0;
}

static void np_diag_write_summary(FILE *f, const RNetSessionStats *st, uint32_t now)
{
    char server_esc[280];
    char lobby_esc[80];
    char bind_esc[80];
    char peer_esc[80];
    char stun_esc[140];
    char turn_esc[140];
    char ice_local_esc[120];
    char ice_remote_esc[120];
    const char *path = np_diag_ice_path(st);
    const char *nat = np_diag_ice_nat(path);
    const char *ice_state =
        st ? rnet_ice_state_name(st->ice_state) : "idle";

    np_diag_escape(server_esc, sizeof(server_esc), g_np.lobby_server);
    np_diag_escape(lobby_esc, sizeof(lobby_esc), g_np.lobby_id);
    np_diag_escape(bind_esc, sizeof(bind_esc), g_np.bind_hostport);
    np_diag_escape(peer_esc, sizeof(peer_esc), g_np.peer_hostport);
    np_diag_escape(stun_esc, sizeof(stun_esc), g_np.ice_stun_host);
    np_diag_escape(turn_esc, sizeof(turn_esc), g_np.ice_turn_host);
    np_diag_escape(ice_local_esc, sizeof(ice_local_esc),
                   st ? st->ice_local : "");
    np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc),
                   st ? st->ice_remote : "");

    fprintf(f,
            "{\"type\":\"summary\",\"t_ms\":%u,\"match\":\"%s\","
            "\"lobby_server\":\"%s\",\"lobby_id\":\"%s\",\"is_host\":%d,"
            "\"slot\":%d,\"session_id\":%u,\"input_delay\":%d,"
            "\"force_input_relay\":%d,"
            "\"transport\":\"%s\",\"bind\":\"%s\",\"peer\":\"%s\","
            "\"turn_configured\":%d,\"stun_host\":\"%s\",\"stun_port\":%u,"
            "\"turn_host\":\"%s\",\"turn_port\":%u,\"ice_state\":\"%s\","
            "\"ice_path\":\"%s\",\"ice_nat\":\"%s\","
            "\"ice_local\":\"%s\",\"ice_remote\":\"%s\"}\n",
            (unsigned)now, g_np.match_mode[0] ? g_np.match_mode : "unknown",
            server_esc, lobby_esc, g_np.is_host, g_np.local_slot,
            (unsigned)g_np.session_id, g_np.input_delay, g_np.force_input_relay,
            g_np.use_ice ? "ice" : "lan", bind_esc, peer_esc,
            g_np.ice_has_turn ? 1 : 0, stun_esc, g_np.ice_stun_port, turn_esc,
            g_np.ice_turn_port, ice_state ? ice_state : "idle", path, nat,
            ice_local_esc, ice_remote_esc);
}

void psx_netplay_diag_tick(void)
{
    RNetSessionStats st;
    uint32_t now;
    const char *transport;
    const char *ice_state;
    const char *path;

    if (!np_diag_enabled() || !psx_netplay_active() || !g_np.session)
        return;

    rnet_session_get_stats(g_np.session, &st);

    if (!g_diag_summary_written && !np_diag_path_ready(&st))
        return;

    now = np_mono_ms();
    if (g_diag_last_write_ms &&
        (uint32_t)(now - g_diag_last_write_ms) < np_diag_interval_ms() &&
        g_diag_summary_written)
        return;
    g_diag_last_write_ms = now ? now : 1u;

    if (!g_diag_mkdir_done) {
        g_diag_mkdir_done = 1;
#ifdef _WIN32
        _mkdir("saves");
        _mkdir("saves\\netplay");
#else
        mkdir("saves", 0755);
        mkdir("saves/netplay", 0755);
#endif
    }

    if (!g_diag_file || g_diag_file_session != g_np.diag_session) {
        char pathbuf[64];
        if (g_diag_file) {
            fclose(g_diag_file);
            g_diag_file = NULL;
        }
        snprintf(pathbuf, sizeof(pathbuf), "saves/netplay/net_diag.jsonl");
        g_diag_file = fopen(pathbuf, "wb");
        if (!g_diag_file)
            return;
        setvbuf(g_diag_file, NULL, _IOLBF, 0);
        g_diag_file_session = g_np.diag_session;
        g_diag_summary_written = 0;
        fprintf(stderr, "psx_netplay: diag writing %s "
                        "(PSX_NET_DIAG_HZ interval %ums)\n",
                pathbuf, np_diag_interval_ms());
    }

    if (!g_diag_summary_written) {
        np_diag_write_summary(g_diag_file, &st, now);
        g_diag_summary_written = 1;
    }

    {
        char ice_local_esc[120];
        char ice_remote_esc[120];
        const char *stall = rnet_admit_stall_name(st.last_stall);
        int using_turn_path = (strcmp(np_diag_ice_path(&st), "relay") == 0) ? 1 : 0;

        transport = psx_netplay_transport_name();
        ice_state = rnet_ice_state_name(st.ice_state);
        path = np_diag_ice_path(&st);
        np_diag_escape(ice_local_esc, sizeof(ice_local_esc), st.ice_local);
        np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc), st.ice_remote);

        fprintf(g_diag_file,
                "{\"t_ms\":%u,\"slot\":%d,\"transport\":\"%s\",\"ice_state\":\"%s\","
                "\"ice_path\":\"%s\",\"ice_nat\":\"%s\",\"turn\":%d,"
                "\"ice_local\":\"%s\",\"ice_remote\":\"%s\","
                "\"running\":%d,\"sim_tick\":%u,\"frames_finished\":%u,"
                "\"delay\":%u,\"stall\":\"%s\","
                "\"stall_ms\":%u,\"stall_max_ms\":%u,\"stall_streaks\":%u,"
                "\"consec_stalls\":%u,\"admit_ok\":%u,\"remote_lead\":%d,"
                "\"remote_wire\":%u,\"peer_rx_age_ms\":%llu,\"peer_gone\":%d,"
                "\"desync\":%d,\"desync_tick\":%u,\"state_busy\":%d,\"state_op\":%u,"
                "\"pkts_rx\":%u,\"input_sends\":%u}\n",
                (unsigned)now, g_np.local_slot, transport ? transport : "none",
                ice_state ? ice_state : "idle", path, np_diag_ice_nat(path),
                using_turn_path, ice_local_esc, ice_remote_esc, st.is_running,
                (unsigned)st.sim_tick, (unsigned)g_np.frames_finished,
                (unsigned)st.delay, stall ? stall : "unknown",
                (unsigned)st.last_admit_wait_ms, (unsigned)st.max_admit_wait_ms,
                (unsigned)st.stall_streaks, (unsigned)st.consecutive_stalls,
                (unsigned)st.admit_ok_count, st.remote_lead,
                (unsigned)st.highest_remote_wire,
                (unsigned long long)st.last_peer_rx_age_ms, st.peer_gone,
                st.input_desync, (unsigned)st.desync_tick, st.state_busy,
                (unsigned)st.state_op, (unsigned)st.packets_rx,
                (unsigned)st.input_bundle_sends);
    }
}

/* Stage the coord save once sim_tick reaches the agreed target. */
static void np_maybe_stage_target_save(void)
{
    uint32_t sim;
    if (g_np.xfer != NP_XFER_SAVE_COORD || g_np.local_save_staged)
        return;
    if (!g_np.session || !rnet_session_is_running(g_np.session))
        return;
    sim = rnet_session_sim_tick(g_np.session);
    if (sim < g_np.save_target_tick)
        return;
    if (!savestate_request_save_protocol(g_np.xfer_slot))
        return;
    g_np.local_save_staged = 1;
    printf("psxrecomp: netplay %s save slot=%d — staging @ sim=%u (target=%u)\n",
           g_np.local_slot == 0 ? "host" : "guest", g_np.xfer_slot,
           (unsigned)sim, (unsigned)g_np.save_target_tick);
    fflush(stdout);
}

static void np_pump_session(void)
{
#if defined(PSX_HAS_LOBBY_CLIENT)
    if (g_np.use_ice || psx_lobby_connected())
        psx_lobby_pump();
#endif
    drain_lobby_signals();
    rnet_session_pump(g_np.session);
    np_drain_peer_frame_commits();
    if (g_np.rollback) {
        np_rollback_reconcile_wire();
        psx_netplay_rb_pump();
    }
    np_guest_handle_probe();
    np_maybe_stage_target_save();
    np_apply_ready_state();
    np_drive_load_barrier();
    np_host_drive_xfer();
    if (rnet_session_is_running(g_np.session))
        np_maybe_start_mc_sync();
}

void psx_netplay_pump(void)
{
    if (!psx_netplay_active())
        return;
    /* Mid-guest cycle-watchdog pump during Replay must not run reconcile /
     * rb_pump (seal/baseline apply, hist promote) — that is host-asymmetric
     * work and was a candidate for SIO fsm forks with matched guest cycles.
     * Still drain transport + FRAME_COMMIT so mid-resim core aborts stay live.
     * Full pump resumes at admit / present edges. */
    if (g_np.rollback && psx_netplay_rb_is_resimulating()) {
#if defined(PSX_HAS_LOBBY_CLIENT)
        if (g_np.use_ice || psx_lobby_connected())
            psx_lobby_pump();
#endif
        drain_lobby_signals();
        rnet_session_pump(g_np.session);
        np_drain_peer_frame_commits();
        /* Stall abort only — full rb_pump stays off mid-guest (asymmetric). */
        psx_netplay_rb_poll_replay_stall();
        return;
    }
    np_pump_session();
    psx_netplay_diag_tick();
}

static int np_try_admit_gameplay(void)
{
    rnet_u32 sim = rnet_session_sim_tick(g_np.session);
    if (rnet_session_try_admit(g_np.session, sim)) {
        g_np.needs_advance = 1;
        return 1;
    }
    force_session_pads_connected(g_np.slot_count);
    return 0;
}

int psx_netplay_poll_admit(void)
{
    rnet_u32 sim;
    int enter_need;
    int exit_need;

    if (!psx_netplay_active()) return 1;

    np_pump_session();

    if (!rnet_session_is_running(g_np.session)) {
        psx_netplay_release_pads();
        np_starv_reset();
        psx_netplay_diag_tick();
        return 0;
    }

    /* Both peers stall until initial memcard hash-agree / transfer finishes. */
    if (!g_np.mc_sync_done)
        return 0;

    /* Post-load: allow admit only while savestate_poll still needs guest
     * cycles. After restore (or during ready rendezvous) freeze the sim clock
     * so peers cannot drift before hard_resync + prime. */
    if (g_np.xfer == NP_XFER_LOAD_APPLYING && !savestate_pending())
        return 0;

    /* Staged load must run guest cycles — bypass starvation latch. ICE xfer
     * often leaves lead=D-1 and would otherwise block try_admit forever. */
    if (g_np.xfer == NP_XFER_LOAD_APPLYING && savestate_pending()) {
        if (g_np.needs_advance)
            return 1;
        return np_try_admit_gameplay();
    }

    /* Both peers: after mutual ready + sync, stay in LOAD_READY until try_admit
     * succeeds (fresh tip exchange + INPUT_CONFIRM). Dropping the barrier early
     * on the host let it spin on confirm with FPS/present already "live". */
    if (g_np.xfer == NP_XFER_LOAD_READY) {
        if (g_np.load_sync_done && g_np.load_ready_replied && !g_np.needs_advance) {
            sim = rnet_session_sim_tick(g_np.session);
            if (rnet_session_try_admit(g_np.session, sim)) {
                g_np.xfer = NP_XFER_NONE;
                g_np.load_applied_local = 0;
                g_np.load_ready_replied = 0;
                g_np.load_sync_done = 0;
                g_np.needs_advance = 1;
                printf("psxrecomp: netplay load slot=%d — peer ready, resuming lockstep\n",
                       g_np.xfer_slot);
                fflush(stdout);
                return 1;
            }
            force_session_pads_connected(g_np.slot_count);
        }
        return 0;
    }

    /* Rollback episode OR pending Live realign: the live needs_advance latch
     * must not bypass rb_try_admit. Resim finish_frame never cleared
     * g_np.needs_advance, so a follower that entered an episode mid-tick spun
     * forever at uncapped FPS without ever arming rb finish_frame. Also stall
     * while a post-abort realign load is queued (episode already inactive). */
    if (g_np.rollback && g_np.xfer == NP_XFER_NONE &&
        (psx_netplay_rb_active() || psx_netplay_rb_load_pending())) {
        if (psx_netplay_rb_tip_holding() && !psx_netplay_rb_load_pending()) {
            /* TipHold: Live invent continues; episode stays open for tip-extend. */
        } else {
            g_np.needs_advance = 0;
            return psx_netplay_rb_try_admit();
        }
    }

    /* Already published this tick and waiting for finish_frame — do not
     * re-admit / re-sample (would desync the delay rings). */
    if (g_np.needs_advance) return 1;

    /* Rollback gameplay: invent missing remotes; skip delay-sync try_admit.
     * Save/load/memcard xfer paths above still use delay-sync admit. */
    if (g_np.rollback && g_np.xfer == NP_XFER_NONE)
        return np_try_admit_rollback();

    sim = rnet_session_sim_tick(g_np.session);
    enter_need = np_starv_env_int("PSX_NET_STARVATION_ENTER_FRAMES",
                                  PSX_STARVATION_ENTER_DEFAULT);
    exit_need = np_starv_env_int("PSX_NET_STARVATION_EXIT_FRAMES",
                                 PSX_STARVATION_EXIT_DEFAULT);

    /* SAVE coord: run admit until both reach save_target_tick and flush the
     * staged write. After the local .pst exists, freeze (host also freezes
     * unless the guest tip is behind and still needs catch-up admits). */
    if (g_np.xfer == NP_XFER_SAVE_COORD) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        np_maybe_stage_target_save();
        if (!g_np.local_save_staged || savestate_pending())
            return np_try_admit_gameplay();
        if (g_np.local_slot != 0)
            return 0; /* guest saved — wait for hash probe */
        /* Host saved: freeze for same-tick match. If guest is still behind
         * the target, keep admitting so it can catch up and write. */
        if (psx_netplay_remote_lead() < 0)
            return np_try_admit_gameplay();
        return 0;
    }

    if (g_np.xfer == NP_XFER_LOAD_PROBE || g_np.xfer == NP_XFER_LOAD_SEND ||
        g_np.xfer == NP_XFER_SAVE_PROBE || g_np.xfer == NP_XFER_SAVE_SEND ||
        g_np.xfer == NP_XFER_MC_PROBE || g_np.xfer == NP_XFER_MC_SEND) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        return np_try_admit_gameplay();
    }

    /* Startup grace: do not latch before the delay rings warm up. */
    if (sim < (rnet_u32)PSX_STARVATION_GRACE_TICKS) {
        g_starv.enter_run = 0;
        g_starv.exit_run = 0;
        g_starv.latched = 0;
        g_starv.just_cleared = 0;
        return np_try_admit_gameplay();
    }

    if (g_starv.latched) {
        /* Pump already ran; hold try_admit until remote tip refills. */
        if (np_starv_runway_ok()) {
            g_starv.exit_run++;
            if (g_starv.exit_run >= exit_need) {
                g_starv.latched = 0;
                g_starv.exit_run = 0;
                g_starv.latch_logged = 0;
                g_starv.just_cleared = 1;
            } else {
                return 0;
            }
        } else {
            g_starv.exit_run = 0;
            return 0;
        }
    }

    if (np_try_admit_gameplay()) {
        g_starv.enter_run = 0;
        if (g_starv.just_cleared) {
            int burst = np_starv_env_int("PSX_NET_STARVATION_RECOVERY_BURST",
                                         PSX_STARVATION_RECOVERY_BURST_DEFAULT);
            g_starv.just_cleared = 0;
            g_starv.recovery_amount = burst;
            if (np_delay_sync_diag_enabled()) {
                if (burst > 0) {
                    fprintf(stderr,
                            "psxrecomp: delay_sync_starvation cleared sim=%u lead=%d "
                            "D=%d — recovery burst %d\n",
                            (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                            psx_netplay_input_delay(), burst);
                } else {
                    fprintf(stderr,
                            "psxrecomp: delay_sync_starvation cleared sim=%u lead=%d "
                            "D=%d — resume 1:1 (rebuild input buffer)\n",
                            (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                            psx_netplay_input_delay());
                }
            }
        }
        return 1;
    }

    g_starv.just_cleared = 0;
    g_starv.enter_run++;
    if (g_starv.enter_run >= enter_need) {
        g_starv.latched = 1;
        g_starv.enter_run = 0;
        if (!g_starv.latch_logged) {
            if (np_delay_sync_diag_enabled()) {
                fprintf(stderr,
                        "psxrecomp: delay_sync_starvation latched sim=%u lead=%d "
                        "D=%d (enter=%d)\n",
                        (unsigned)psx_netplay_sim_tick(), psx_netplay_remote_lead(),
                        psx_netplay_input_delay(), enter_need);
            }
            g_starv.latch_logged = 1;
        }
    }
    return 0;
}

void psx_netplay_finish_frame(void)
{
    rnet_u32 done;
    if (!psx_netplay_active()) return;

    if (g_np.rollback && psx_netplay_rb_is_resimulating()) {
        rnet_u32 done = rnet_session_sim_tick(g_np.session);
        /* Present-edge only: mid-guest pump skips reconcile. Coalesce late
         * wire into the active episode (tip-extend) before POST/Verify. */
        rnet_session_pump(g_np.session);
        np_rollback_reconcile_wire();
        psx_netplay_rb_pump();
        psx_netplay_rb_finish_frame();
        /* Exchange cores during Replay so mid-resim forks abort before POST. */
        np_emit_frame_commit(done);
        np_drain_peer_frame_commits();
        g_np.latched_for_tick = 0;
        g_np.needs_advance = 0; /* live latch must not outlive a resim vblank */
        g_np.frames_finished++;
        return;
    }

    if (!g_np.needs_advance) return;
    /* Digest the tick that just ran (sim_tick before advance). */
    done = rnet_session_sim_tick(g_np.session);
    np_emit_frame_commit(done);
    if (g_np.rollback) {
        uint32_t resume_hint = 0;
        /* TipHold Live: reconcile late wire so tip-extend can schedule rereplay. */
        if (psx_netplay_rb_tip_holding())
            np_rollback_reconcile_wire();
        psx_netplay_rb_pump();
        psx_netplay_rb_request_snap(done);
        /* Flush at vblank: MotK IRQ fast/mid paths used to skip poll_snap, so
         * deferred BB-edge saves never ran and the ring stayed empty. Prefer
         * IRQ BB-edge PCs — cpu->pc is often 0 during present/finish_frame. */
        if (g_np.cpu) {
            resume_hint = psx_compiled_irq_resume_pc();
            if (!psx_is_dispatchable(resume_hint))
                resume_hint = psx_last_irq_check_pc();
            if (!psx_is_dispatchable(resume_hint))
                resume_hint = g_np.cpu->pc;
            psx_netplay_rb_poll(g_np.cpu, resume_hint);
        }
    }
    rnet_session_advance(g_np.session);
    /* DELAY_SYNC may commit on this advance — keep g_np.input_delay aligned. */
    np_sync_input_delay_from_session();
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.frames_finished++;
}

void psx_netplay_bind_cpu(struct CPUState *cpu)
{
    g_np.cpu = (CPUState*)cpu;
}

uint32_t psx_netplay_resolved_through(void)
{
    if (!psx_netplay_active()) return 0;
    return netplay_hc_resolved_through(&g_np.hc);
}

int psx_netplay_hash_confirm_through(uint32_t tick)
{
    if (!psx_netplay_active()) return 0;
    return netplay_hc_confirm_through(&g_np.hc, tick) ? 1 : 0;
}

int psx_netplay_rollback_mode(void)
{
    return psx_netplay_active() && g_np.rollback;
}

void psx_netplay_poll_snap(struct CPUState *cpu, uint32_t resume_pc)
{
    if (!psx_netplay_active() || !g_np.rollback)
        return;
    psx_netplay_rb_poll(cpu, resume_pc);
    /* BB-edge (interrupts.c): safe to longjmp like savestate_poll. */
    psx_netplay_rb_flush_resume();
}

int psx_netplay_is_resimulating(void)
{
    return psx_netplay_active() && g_np.rollback && psx_netplay_rb_is_resimulating();
}

int psx_netplay_remote_lead(void)
{
    RNetSessionStats st;
    if (!psx_netplay_active())
        return 0;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.remote_lead;
}

int psx_netplay_input_delay(void)
{
    RNetSessionStats st;
    if (!psx_netplay_active())
        return 2;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.delay > 0 ? (int)st.delay : 2;
}

int psx_netplay_catchup_budget(void)
{
    int lead;
    int delay;
    int extra;
    int budget;
    int cap;

    if (!psx_netplay_active())
        return 0;
    cap = np_starv_env_int("PSX_NET_CATCHUP_CAP", PSX_CATCHUP_CAP_DEFAULT);
    if (cap <= 0 && g_starv.recovery_amount <= 0)
        return 0;
    lead = psx_netplay_remote_lead();
    delay = psx_netplay_input_delay();
    if (delay < 0)
        delay = 0;
    /* Only spend surplus above D; keep the delay runway intact. */
    extra = lead - delay;
    if (extra < 0)
        extra = 0;
    budget = extra;
    if (g_starv.recovery_amount > budget)
        budget = g_starv.recovery_amount;
    if (budget > cap)
        budget = cap;
    return budget;
}

void psx_netplay_catchup_consume_frame(void)
{
    if (g_starv.recovery_amount > 0)
        g_starv.recovery_amount--;
}

void psx_netplay_wait_recv(int timeout_ms)
{
    if (!psx_netplay_active()) return;
    (void)rnet_session_wait_recv(g_np.session, timeout_ms);
}

void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out)
{
    RNetSessionStats st;
    const char *name = "inactive";
    char phase[96];
    memset(&st, 0, sizeof(st));
    phase[0] = '\0';
    if (psx_netplay_active() && g_np.session) {
        rnet_session_get_stats(g_np.session, &st);
        name = rnet_admit_stall_name(st.last_stall);
        if (!name || !name[0])
            name = "unknown";
        /* LOAD_READY never calls try_admit, so last_stall stays "ok" — surface
         * the app barrier phase (+ transfer progress) instead. */
        switch (g_np.xfer) {
        case NP_XFER_SAVE_COORD:
            snprintf(phase, sizeof(phase), "save_coord");
            break;
        case NP_XFER_SAVE_PROBE:
            snprintf(phase, sizeof(phase), "save_probe");
            break;
        case NP_XFER_SAVE_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "save_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "save_xfer");
            break;
        case NP_XFER_MC_PROBE:
            snprintf(phase, sizeof(phase), "mc_probe");
            break;
        case NP_XFER_MC_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "mc_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "mc_xfer");
            break;
        case NP_XFER_LOAD_PROBE:
            snprintf(phase, sizeof(phase), "load_probe");
            break;
        case NP_XFER_LOAD_SEND:
            if (st.state_bytes_total > 0)
                snprintf(phase, sizeof(phase), "load_xfer_%u/%u",
                         (unsigned)st.state_bytes_acked, (unsigned)st.state_bytes_total);
            else
                snprintf(phase, sizeof(phase), "load_xfer");
            break;
        case NP_XFER_LOAD_APPLYING:
            if (savestate_pending()) {
                if (g_starv.latched)
                    snprintf(phase, sizeof(phase), "load_applying+starv_%s", name);
                else
                    snprintf(phase, sizeof(phase), "load_applying+%s", name);
            } else {
                snprintf(phase, sizeof(phase), "load_apply_done+%s", name);
            }
            break;
        case NP_XFER_LOAD_READY:
            if (g_np.load_ready_replied)
                snprintf(phase, sizeof(phase), "load_ready_admit+%s", name);
            else if (g_np.load_applied_local)
                snprintf(phase, sizeof(phase), "load_ready_wait_peer+%s", name);
            else
                snprintf(phase, sizeof(phase), "load_ready+%s", name);
            break;
        default:
            break;
        }
        /* Rollback episode: try_admit is skipped so last_stall stays "ok" —
         * surface the RB FSM phase instead. */
        if (!phase[0] && g_np.rollback && psx_netplay_rb_active()) {
            static const char *const k_rb_phase[] = {
                "rb_live", "rb_seal", "rb_baseline", "rb_replay",
                "rb_verify", "rb_commit", "rb_abort"
            };
            int ph = psx_netplay_rb_phase();
            if (ph >= 0 && ph < (int)(sizeof(k_rb_phase) / sizeof(k_rb_phase[0])))
                snprintf(phase, sizeof(phase), "%s", k_rb_phase[ph]);
            else
                snprintf(phase, sizeof(phase), "rb_phase_%d", ph);
        }
    }
    if (stall_out && stall_cap) {
        if (phase[0])
            snprintf(stall_out, stall_cap, "%s", phase);
        else {
            strncpy(stall_out, name, stall_cap - 1);
            stall_out[stall_cap - 1] = '\0';
        }
    }
    if (sim_tick_out)
        *sim_tick_out = st.sim_tick;
    if (lead_out)
        *lead_out = st.remote_lead;
}

#endif /* PSX_HAS_RECOMP_NET */
