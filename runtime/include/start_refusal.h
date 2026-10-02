/* start_refusal.h — why a start ended before the first frame (PS1G-63).
 *
 * A start that is refused (a BIOS this build was not compiled from, no disc,
 * an image the drive cannot mount) returns from main() before any guest code
 * runs. The run report then read reason "atexit", exit_origin "unknown",
 * frame 0 for every one of those causes, and for a closed launcher window
 * too, so a player's report could not say why the game did not start.
 *
 * This module keeps the kind of the refusal and the sentence the player was
 * shown. crash_trace.c prints both into psx_last_run_report.json. It also
 * keeps what the launcher's BIOS and disc rows said last, because a launcher
 * that blocks Play ends as "launcher closed" with the reason on those rows.
 *
 * Fixed buffers, no allocation: the report writer also runs on the crash path.
 */
#ifndef PSXRECOMP_START_REFUSAL_H
#define PSXRECOMP_START_REFUSAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_START_REFUSAL_KIND_CAP  48
#define PSX_START_REFUSAL_TITLE_CAP 96
#define PSX_START_REFUSAL_TEXT_CAP  1024
#define PSX_START_LAUNCHER_ROW_CAP  320

/* Record that this start is refused. `kind` is a short stable word for tools
 * ("no_bios", "disc_not_mounted"); `title` and `message` are what the player
 * was shown. Longer text is cut at the cap. The first refusal of a start is
 * kept: a later exit path must not replace the reason. */
void psx_start_refusal_set(const char *kind, const char *title, const char *message);

/* The kind of the recorded refusal, or NULL when the start was not refused. */
const char *psx_start_refusal_kind(void);

/* What a launcher row said last. `row` is "bios" or "disc"; other names are
 * ignored. An empty text clears the row. */
void psx_start_note_launcher(const char *row, const char *text);

/* The report values, as JSON text: `null`, or an object.
 *   start_refused:   {"kind": "...", "title": "...", "message": "..."}
 *   launcher_status: {"bios": "...", "disc": "..."}
 * Each writes at most cap - 1 characters and a terminator, and returns the
 * length written. A buffer too small for the value gets `null`. */
int psx_start_refusal_json(char *out, size_t cap);
int psx_start_launcher_status_json(char *out, size_t cap);

/* Forget everything (tests, and a session that goes back to the launcher). */
void psx_start_refusal_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_START_REFUSAL_H */
