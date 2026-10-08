/* psx_cli_tail.h -- why a run of the CLI failed, in words for the setup window.
 *
 * Last-lines capture so a failed CLI run can say WHY in the wizard, which has
 * no console: the child's stderr shares the progress pipe, JSON progress rows
 * contribute their "message", raw rows (compiler/traceback text) contribute
 * as-is, and the most recent error-looking line is appended to err_msg. This
 * is what turns "psxrecomp rebuild failed (exit 1)" into "... failed (exit 1):
 * Could NOT find OpenGL (missing: OPENGL_INCLUDE_DIR)".
 *
 * Header-only and free of the host's other code, so a test can compile it.
 */
#ifndef PSX_CLI_TAIL_H
#define PSX_CLI_TAIL_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "psx_json_text.h"

typedef struct {
    char last[480];
    char last_err[480];
} CliTail;

static int cli_tail_line_is_error(const char* s) {
    return strstr(s, "rror") != NULL || strstr(s, "ailed") != NULL ||
           strstr(s, "FAILED") != NULL || strstr(s, "Traceback") != NULL ||
           strstr(s, "fatal") != NULL || strstr(s, "Fatal") != NULL;
}

static void cli_tail_note(CliTail* t, const char* line) {
    char msg[480];
    const char* rec = line;
    int is_error_event = 0;
    if (line[0] == '{') {
        /* sdk_progress emits compact JSON: {"event":"error","message":...}. */
        is_error_event = strstr(line, "\"event\":\"error\"") != NULL;
        if (!json_get_string(line, "message", msg, sizeof(msg)))
            return; /* structured row without text (e.g. result) */
        rec = msg;
    }
    if (!rec[0])
        return;
    snprintf(t->last, sizeof(t->last), "%s", rec);
    if (is_error_event || cli_tail_line_is_error(rec))
        snprintf(t->last_err, sizeof(t->last_err), "%s", rec);
}

static void cli_fail_msg(char* err_msg, size_t err_cap, const char* fail_label,
                         long code, const CliTail* t) {
    const char* why = t->last_err[0] ? t->last_err : t->last;
    if (code == 3) {
        /* The CLI names the failing check (a digest mismatch, or a .chd it
         * cannot read) and cli_tail_note has captured that line. A flat
         * "wrong dump" contradicts it and sends players hunting a bad rip
         * when the dump is fine. Keep the reason when there is one. */
        if (why[0])
            snprintf(err_msg, err_cap, "Disc verification failed: %s", why);
        else
            snprintf(err_msg, err_cap, "Disc verification failed (wrong dump).");
        return;
    }
    if (why[0])
        snprintf(err_msg, err_cap, "%s failed (exit %ld): %s", fail_label,
                 code, why);
    else
        snprintf(err_msg, err_cap, "%s failed (exit %ld).", fail_label, code);
}

#endif /* PSX_CLI_TAIL_H */
