#ifndef PSX_NETPLAY_RB_H
#define PSX_NETPLAY_RB_H

/*
 * MotK rollback episode + snap-ring host (PSX_NET_MODE=rollback).
 * Delay-sync path never calls these.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
struct RNetSession;
struct NetplayInputHist;
struct NetplayHashConfirm;

typedef struct PsxNetplayRbBindings {
    struct RNetSession **session;
    struct CPUState **cpu;
    struct NetplayInputHist *ih;
    struct NetplayHashConfirm *hc;
    uint32_t *bios_checksum;
    uint32_t *entry_pc;
    int *slot_count;
    int *local_slot;
    int *input_delay;
    void (*publish_sio)(uint32_t tick); /* publish history or sealed rows to SIO */
    void (*apply_frame_slot)(int slot, uint32_t tick, uint16_t buttons,
                             int8_t sx, int8_t sy, uint8_t analog);
} PsxNetplayRbBindings;

void psx_netplay_rb_bind(const PsxNetplayRbBindings *b);
void psx_netplay_rb_start(void);
void psx_netplay_rb_shutdown(void);

/* Safe BB-edge poll (mirror savestate_poll). Saves pending snap / applies load
 * without longjmp. Call psx_netplay_rb_flush_resume() afterward from a C BB-edge
 * (or after all C++ RAII in the vblank path has been destroyed). */
void psx_netplay_rb_poll(struct CPUState *cpu, uint32_t resume_pc);

/* If a baseline snap was applied without resume, longjmp to the scheduler
 * (same as savestate_poll). No-op when nothing is pending. Never returns on
 * success. */
void psx_netplay_rb_flush_resume(void);

/* After a live tick completes: request snap at tick for next safe poll. */
void psx_netplay_rb_request_snap(uint32_t tick);

/* Initiator: start episode from invent/contract rewind.
 * Returns 1 if an episode opened, 0 if refused (no snap, already active,
 * post-commit/realign cooldown, …). On refuse, reconcile should promote wire. */
int  psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot);

/* 1 while begin_rewind is suppressed (cooldown after commit / realign). */
int  psx_netplay_rb_rewind_suppressed(void);

/* 1 during depth24 / recent MDEC / post-FMV settle — MotK FMV. Reconcile
 * promotes wire; no episodes (CD-frozen Replay corrupts the movie). */
int  psx_netplay_rb_fmv_defer_rewind(void);

/* 1 in the same FMV/settle window: admit must stall for remote wire instead
 * of inventing (promote-only during FMV left peers desynced at cutover). */
int  psx_netplay_rb_lockstep_no_invent(void);

/* 1 once after commit/abort/realign — reconcile should promote all late wire
 * without opening another episode (clears invent poison). */
int  psx_netplay_rb_take_promote_sweep(void);

/* Drain peer RB_* + drive Seal/Baseline/Replay/Verify. Call from pump. */
void psx_netplay_rb_pump(void);

/* 1 while episode is active (seal/baseline/replay/verify). */
int  psx_netplay_rb_active(void);
int  psx_netplay_rb_is_resimulating(void);

/* FRAME_COMMIT mismatch during Replay — abort before a false POST commit.
 * Returns 1 if an episode was aborted. */
int  psx_netplay_rb_abort_resim_core_mismatch(uint32_t tick, uint32_t local_core,
                                              uint32_t peer_core);
/* 1 while a baseline/realign snap load is queued or resume is deferred —
 * poll_admit must stall (do not run live invent). */
int  psx_netplay_rb_load_pending(void);

/*
 * During resim: publish sealed inputs for current sim tick and admit.
 * Returns 1 if guest should run this tick, 0 to wait (seal/baseline).
 */
int  psx_netplay_rb_try_admit(void);

/* After guest frame during resim: advance episode clock; may commit. */
void psx_netplay_rb_finish_frame(void);

/* Present-edge digest prep: copy `in` → `out` and clear PC. At vblank
 * finish_frame the host often parks cpu->pc=0 while a peer may still hold a
 * live BB PC; sticky substitutes were host-local and forked dig_cpu with
 * matched RAM/clk. GPRs/COP0/cycles remain. */
void psx_netplay_rb_cpu_for_present_digest(struct CPUState *out,
                                           const struct CPUState *in);

/* Diag */
uint32_t psx_netplay_rb_episode_count(void);
int      psx_netplay_rb_phase(void); /* RNetRbPhase cast to int */
uint32_t psx_netplay_rb_snap_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_RB_H */
