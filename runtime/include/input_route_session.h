#ifndef PSX_INPUT_ROUTE_SESSION_H
#define PSX_INPUT_ROUTE_SESSION_H
/* Route identity, markers and replay outside the debug server (T101).
 *
 * Replay (every product): PSX_INPUT_ROUTE_FILE names a PSXRTI1, PSXRTI2 or
 * PSXRTI3 route. A PSXRTI3 identity block is compared with this product
 * before guest execution and a mismatch refuses the launch. PSXRTI3 markers
 * are checked at their frame boundary: guest cycle, main-RAM SHA-256 and
 * page hashes are printed and compared with the recorded checkpoint.
 * The diagnostic product keeps its debug-server replay path; the release
 * product replays through this module. Every entry point is inert when no
 * route or recording is armed.
 *
 * Pictures (release product, PS1B-404): with PSX_INPUT_ROUTE_CAPTURE_DIR set
 * and a route admitted, the release replay drives the route observer
 * (input_route_observer.c) that the diagnostic product drives from its debug
 * server. It writes a picture every PSX_INPUT_ROUTE_CAPTURE_EVERY boundaries
 * and at the route's end, where the process exits with status 0. Without the
 * directory, a loaded route costs one more branch per vblank.
 *
 * Record (diagnostic product): PSX_INPUT_ROUTE_RECORD names a new PSXRTI3
 * file written at process exit. One digital P1 word is recorded per guest
 * vblank from boot, with identity, markers and checkpoints. */
#include <stdint.h>
#include <stdio.h>
#include "input_route_v3_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Product identity, set before admission. The disc digest is computed only
 * when a route or recording needs it. */
void input_route_session_set_product(const char *disc_serial,
                                     const char *disc_path,
                                     const char *bios_path);
/* The disc in the drive changed under the running product (an in-game disc
 * change, or a save state that mounted its own disc): the identity names
 * this disc from here on. The BIOS stays. Cached digests of the previous
 * disc are dropped. */
void input_route_session_set_disc(const char *disc_serial,
                                  const char *disc_path);

/* Prestart admission of PSX_INPUT_ROUTE_FILE. Parses the whole file and arms
 * markers and the declared P1 device. Returns 0 and prints the reason when
 * the route is refused. */
int input_route_session_admit(const char *path);
/* Diagnostic product: begin recording to `path`. Returns 0 when refused. */
int input_route_session_record_begin(const char *path);
/* After the BIOS HLE plan is fixed: compare the admitted identity (replay) or
 * capture it (record). Returns 0 and prints both sides on a mismatch. */
int input_route_session_verify_identity(int call_hle, int boot_skip);
/* Hashes and caches the disc digest now; 1 on success. Not thread-safe by
 * itself: the host serialises it with set_product and identity. */
int input_route_session_prefetch_disc_digest(void);

/* Debug-server preload of a PSXRTI3 body (diagnostic product). */
const char *input_route_session_read_v3_digital(FILE *f, InputRouteStep *steps,
                                                uint32_t *step_count,
                                                uint32_t *frame_count);
const char *input_route_session_read_v3_dualshock(FILE *f,
                                                  InputDualShockRouteStep *steps,
                                                  uint32_t *step_count,
                                                  uint32_t *frame_count);

/* Identity of this product for a player replay (PS1B-191): pin, disc serial
 * and digest, BIOS stem and boot mode. The disc digest is hashed on first use
 * and cached. Returns 0 and a reason when the product has no disc or BIOS. */
int input_route_session_identity(InputRouteV3 *meta, int call_hle, int boot_skip,
                                 char *why, size_t why_size);

/* True while a PSXRTI3 route or a recording declares the controller ports:
 * one digital pad on P1, nothing else. Host hotplug must not change them. */
int input_route_session_owns_ports(void);
/* True while any input route is admitted (PSXRTI1, PSXRTI2 or PSXRTI3,
 * digital or DualShock) or a recording is running. */
int input_route_session_armed(void);

/* Once per guest vblank, before the input for the next record is taken.
 * Returns -1 normally, or a process exit status when the last marker was
 * checked and PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS=1. */
int input_route_session_boundary(void);

/* Release product: the next route word, or -1 when no route is loaded. */
int input_route_session_release_override(void);
/* Release product: deliver a DualShock route input. 0 when not in that mode. */
int input_route_session_release_dualshock(int buttons);

/* Recording (diagnostic product). */
int input_route_session_recording(void);
void input_route_session_record_input(uint16_t buttons);
/* kind: INPUT_ROUTE_MARKER_MENU or INPUT_ROUTE_MARKER_GAMEPLAY. Returns 0
 * when not recording. The marker lands on the next frame boundary. */
int input_route_session_request_marker(unsigned kind);
/* JSON object body (no braces) describing recorder state. */
void input_route_session_record_status(char *out, size_t size);

#ifdef __cplusplus
}
#endif
#endif
