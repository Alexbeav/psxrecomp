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

/* A text that was cut to fit its room can end inside a UTF-8 character: the
 * messages are UTF-8 since the reader resolves the CLI's escapes (PS1B-413),
 * and snprintf cuts at a byte. Take such a half character off the end, so the
 * setup window never gets one. A text that ends on a whole character, and a
 * text that is not UTF-8 at its end, stay as they are. */
static void psx_text_drop_cut_character(char* s) {
    const size_t n = strlen(s);
    size_t tail = 0; /* continuation bytes (10xxxxxx) at the end, at most 3 */
    while (tail < n && tail < 3 &&
           ((unsigned char)s[n - 1 - tail] & 0xC0u) == 0x80u)
        ++tail;
    if (tail == n)
        return;
    {
        const unsigned char lead = (unsigned char)s[n - 1 - tail];
        const size_t whole =
            lead >= 0xF0u ? 4 : lead >= 0xE0u ? 3 : lead >= 0xC0u ? 2 : 1;
        if (whole > 1 && tail + 1 < whole)
            s[n - 1 - tail] = '\0';
    }
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
    psx_text_drop_cut_character(t->last);
    if (is_error_event || cli_tail_line_is_error(rec)) {
        snprintf(t->last_err, sizeof(t->last_err), "%s", rec);
        psx_text_drop_cut_character(t->last_err);
    }
}

static void cli_fail_msg(char* err_msg, size_t err_cap, const char* fail_label,
                         long code, const CliTail* t) {
    const char* why = t->last_err[0] ? t->last_err : t->last;
    if (err_cap == 0)
        return;
    if (code == 3) {
        /* The CLI names the failing check (a digest mismatch, or a .chd it
         * cannot read) and cli_tail_note has captured that line. A flat
         * "wrong dump" contradicts it and sends players hunting a bad rip
         * when the dump is fine. Keep the reason when there is one. */
        if (why[0])
            snprintf(err_msg, err_cap, "Disc verification failed: %s", why);
        else
            snprintf(err_msg, err_cap, "Disc verification failed (wrong dump).");
    } else if (why[0]) {
        snprintf(err_msg, err_cap, "%s failed (exit %ld): %s", fail_label,
                 code, why);
    } else {
        snprintf(err_msg, err_cap, "%s failed (exit %ld).", fail_label, code);
    }
    psx_text_drop_cut_character(err_msg);
}

#endif /* PSX_CLI_TAIL_H */
