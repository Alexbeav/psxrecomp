/* start_refusal.c — see start_refusal.h. */
#include "start_refusal.h"

#include <stdio.h>
#include <string.h>

static char s_kind[PSX_START_REFUSAL_KIND_CAP];
static char s_title[PSX_START_REFUSAL_TITLE_CAP];
static char s_message[PSX_START_REFUSAL_TEXT_CAP];
static char s_row_bios[PSX_START_LAUNCHER_ROW_CAP];
static char s_row_disc[PSX_START_LAUNCHER_ROW_CAP];

static void copy_capped(char *dst, size_t cap, const char *src) {
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
}

void psx_start_refusal_set(const char *kind, const char *title, const char *message) {
    if (!kind || !kind[0] || s_kind[0]) return;
    copy_capped(s_kind, sizeof(s_kind), kind);
    copy_capped(s_title, sizeof(s_title), title);
    copy_capped(s_message, sizeof(s_message), message);
}

const char *psx_start_refusal_kind(void) {
    return s_kind[0] ? s_kind : NULL;
}

void psx_start_note_launcher(const char *row, const char *text) {
    if (!row) return;
    if (strcmp(row, "bios") == 0) copy_capped(s_row_bios, sizeof(s_row_bios), text);
    else if (strcmp(row, "disc") == 0) copy_capped(s_row_disc, sizeof(s_row_disc), text);
}

void psx_start_refusal_reset(void) {
    s_kind[0] = s_title[0] = s_message[0] = '\0';
    s_row_bios[0] = s_row_disc[0] = '\0';
}

/* Append `s` as a JSON string. Returns 0 when it does not fit. A line break
 * becomes \n; any other control character becomes a space. */
static int put_json_string(char *out, size_t cap, size_t *pos, const char *s) {
    size_t p = *pos;
    if (p + 1 >= cap) return 0;
    out[p++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\r') continue;
        if (c == '"' || c == '\\' || c == '\n') {
            if (p + 2 >= cap) return 0;
            out[p++] = '\\';
            out[p++] = (char)(c == '\n' ? 'n' : c);
        } else {
            if (p + 1 >= cap) return 0;
            out[p++] = (char)(c < 0x20 ? ' ' : c);
        }
    }
    if (p + 1 >= cap) return 0;
    out[p++] = '"';
    out[p] = '\0';
    *pos = p;
    return 1;
}

static int put_text(char *out, size_t cap, size_t *pos, const char *s) {
    size_t n = strlen(s);
    if (*pos + n >= cap) return 0;
    memcpy(out + *pos, s, n + 1);
    *pos += n;
    return 1;
}

static int put_null(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    return snprintf(out, cap, "%s", cap > 4 ? "null" : "");
}

int psx_start_refusal_json(char *out, size_t cap) {
    size_t pos = 0;
    if (!out || cap == 0) return 0;
    if (!s_kind[0]) return put_null(out, cap);
    if (put_text(out, cap, &pos, "{\"kind\": ") &&
        put_json_string(out, cap, &pos, s_kind) &&
        put_text(out, cap, &pos, ", \"title\": ") &&
        put_json_string(out, cap, &pos, s_title) &&
        put_text(out, cap, &pos, ", \"message\": ") &&
        put_json_string(out, cap, &pos, s_message) &&
        put_text(out, cap, &pos, "}"))
        return (int)pos;
    return put_null(out, cap);
}

int psx_start_launcher_status_json(char *out, size_t cap) {
    size_t pos = 0;
    if (!out || cap == 0) return 0;
    if (!s_row_bios[0] && !s_row_disc[0]) return put_null(out, cap);
    if (put_text(out, cap, &pos, "{\"bios\": ") &&
        put_json_string(out, cap, &pos, s_row_bios) &&
        put_text(out, cap, &pos, ", \"disc\": ") &&
        put_json_string(out, cap, &pos, s_row_disc) &&
        put_text(out, cap, &pos, "}"))
        return (int)pos;
    return put_null(out, cap);
}
