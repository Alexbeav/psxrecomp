#ifndef PSX_REPLAY_SESSION_H
#define PSX_REPLAY_SESSION_H
/* Player replay recorder and player (PS1B-191).
 *
 * A replay is a PSXRTI3 route (input_route_v3_file.h) that also carries its
 * anchor: the machine state it starts from, and the host settings that change
 * guest timing. It records the P1 pad once per vblank as delivered to SIO
 * (buttons and protocol sticks), with an END checkpoint (cycle, RAM hash) on
 * the last boundary, so playback reports whether it stayed in sync.
 *
 * Recording and playback both start from a freshly LOADED anchor: starting a
 * recording saves the state and loads it straight back, so the recorded run
 * and every later playback take the same post-load path.
 *
 * The module owns no guest or host state directly; main.cpp supplies the
 * replay_host_* hooks below (the unit test supplies stubs). */
#include <stddef.h>
#include <stdint.h>
#include "input_route_v3_file.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REPLAY_SLOTS 12

typedef enum {
    REPLAY_IDLE = 0,
    REPLAY_ARMING,     /* anchor save+reload requested */
    REPLAY_RECORDING,
    REPLAY_LOADING,    /* playback anchor load requested */
    REPLAY_PLAYING
} ReplayState;

/* Result of the last playback, for headless tests and the OSD. */
typedef enum {
    REPLAY_RESULT_NONE = 0,
    REPLAY_RESULT_IN_SYNC,
    REPLAY_RESULT_OUT_OF_SYNC,
    REPLAY_RESULT_TAKEN_OVER,
    REPLAY_RESULT_FAILED
} ReplayResult;

ReplayState replay_session_state(void);
ReplayResult replay_session_last_result(void);

/* F11 (HOST_KEYMAP_REPLAY_RECORD): start recording into the next free slot,
 * or stop the recording in progress. Returns 1 when the request was taken. */
int replay_session_toggle_record(void);
/* Start recording into an explicit new file (headless tests). The file must
 * not exist. */
int replay_session_record_to(const char *path);

int replay_session_play_slot(int slot);
int replay_session_play_file(const char *path);

/* Replay slot files: <save dir>/replay_<entry>_slotNN.psxrpl, next to the
 * save-state slots. Returns 1 on success. */
int replay_session_slot_path(int slot, char *out, size_t cap);
int replay_session_slot_exists(int slot);
/* First empty slot, or -1 when every slot holds a replay. */
int replay_session_next_free_slot(void);
int replay_session_delete_slot(int slot);
/* Copy a slot to <save root>/replays/<serial>-<stamp>.psxrpl for sharing. */
int replay_session_export_slot(int slot, char *out_path, size_t cap);

/* 1 while recording or playing: P1 is driven through replay_session_boundary
 * and host pad-type changes are suspended. */
int replay_session_owns_p1(void);

/* Once per guest vblank, at the frame boundary, with the live P1 state the
 * host would deliver. Returns 1 when the host must deliver *out_buttons and
 * out_sticks (LX,LY,RX,RY) to P1 for the next frame instead of live input. */
int replay_session_boundary(uint16_t live_buttons, const uint8_t live_sticks[4],
                            uint16_t *out_buttons, uint8_t out_sticks[4]);

/* REC indicator: 1 while recording and in the visible half of the blink. */
int replay_session_rec_visible(uint64_t now_ms);

/* Rollback state digests (netplay_state_digest.h) taken every
 * REPLAY_DIGEST_INTERVAL frames while recording and compared during playback.
 * CORE (CPU, RAM, clock, IRQ, timers) and EXT (SPU, MDEC, CD, scratchpad, DMA,
 * SIO) decide the result; AV (GPU + VRAM) is reported only, because GL and
 * Vulkan readback fork VRAM while the guest still matches. */
#define REPLAY_DIGEST_INTERVAL 60u
#define REPLAY_DIGEST_CORE 1u
#define REPLAY_DIGEST_AV   2u
#define REPLAY_DIGEST_AUX  4u
#define REPLAY_DIGEST_EXT  8u
/* 1 when the last playback's digests diverged: the first frame and the
 * partitions that differed there. */
int replay_session_first_divergence(uint32_t *frame, unsigned *parts);
unsigned replay_session_digests_checked(void);

/* Machine-readable verdict: when set, every playback that starts writes this
 * JSON file when it ends (result in_sync | diverged | stopped_by_input |
 * failed, frames, first divergence, recorded and player builds). NULL clears. */
void replay_session_set_verdict_path(const char *path);

/* Process exit or a disc change: finish a recording from the last complete
 * boundary, and end a playback (settings restored). */
void replay_session_shutdown(void);

/* ---- Host hooks (main.cpp; stubs in the unit test) ---- */
void replay_host_osd(const char *text, int ms);
/* 0 and a reason when recording is not possible now (netplay, a second
 * controller, an armed route). */
int replay_host_can_record(char *why, size_t cap);
/* Fill the identity fields of *meta (pin, disc, BIOS, boot mode). */
int replay_host_identity(InputRouteV3 *meta, char *why, size_t cap);
/* Save the machine state and load it straight back at the next safe
 * boundary; the blob is fetched with replay_host_take_anchor. */
int replay_host_request_anchor(void);
/* 1 = done (blob returned, caller frees), -1 = failed, 0 = pending. */
int replay_host_take_anchor(uint8_t **data, size_t *size);
/* Load a state blob at the next safe boundary. */
int replay_host_load_anchor(const void *data, size_t size);
/* 1 = loaded, -1 = failed, 0 = pending. */
int replay_host_take_load_result(void);
/* "key=value\n" lines of the settings that change guest timing. */
void replay_host_settings_capture(char *out, size_t cap);
/* Switch to `settings`; lines that cannot be switched live are reported in
 * `differs` (empty when all applied). */
void replay_host_settings_apply(const char *settings, char *differs, size_t cap);
/* Put back the settings captured before the last apply. */
void replay_host_settings_restore(void);
/* Guest main RAM (2 MiB) and cycle count at the current boundary. */
const uint8_t *replay_host_ram(void);
/* core, av, aux, ext; 0 when no digest is available (none are recorded). */
int replay_host_state_digest(uint32_t out[4]);
uint64_t replay_host_cycle(void);
/* Directory of the save-state slots and the slot-name key ("state_<entry>"
 * style prefix is replaced by "replay_"); the disc serial for exports. */
int replay_host_slot_base(char *dir, size_t dir_cap, char *prefix, size_t prefix_cap);
const char *replay_host_export_dir(void);
const char *replay_host_disc_serial(void);

#ifdef __cplusplus
}
#endif
#endif
