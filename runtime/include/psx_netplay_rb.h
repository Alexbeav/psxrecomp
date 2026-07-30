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
                             int8_t sx, int8_t sy);
} PsxNetplayRbBindings;

void psx_netplay_rb_bind(const PsxNetplayRbBindings *b);
void psx_netplay_rb_start(void);
void psx_netplay_rb_shutdown(void);

/* Safe BB-edge poll (mirror savestate_poll). Saves pending snap / applies load. */
void psx_netplay_rb_poll(struct CPUState *cpu, uint32_t resume_pc);

/* After a live tick completes: request snap at tick for next safe poll. */
void psx_netplay_rb_request_snap(uint32_t tick);

/* Initiator: start episode from invent/contract rewind. */
void psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot);

/* Drain peer RB_* + drive Seal/Baseline/Replay/Verify. Call from pump. */
void psx_netplay_rb_pump(void);

/* 1 while episode is active (seal/baseline/replay/verify). */
int  psx_netplay_rb_active(void);
int  psx_netplay_rb_is_resimulating(void);

/*
 * During resim: publish sealed inputs for current sim tick and admit.
 * Returns 1 if guest should run this tick, 0 to wait (seal/baseline).
 */
int  psx_netplay_rb_try_admit(void);

/* After guest frame during resim: advance episode clock; may commit. */
void psx_netplay_rb_finish_frame(void);

/* Diag */
uint32_t psx_netplay_rb_episode_count(void);
int      psx_netplay_rb_phase(void); /* RNetRbPhase cast to int */
uint32_t psx_netplay_rb_snap_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_RB_H */
