/* psx_json_text.h -- read one string value out of a compact JSON row.
 *
 * The setup host reads the CLI's progress rows ({"event":"error","message":
 * "..."}) and shows the message in the setup window. The CLI writes ASCII
 * JSON, so a letter outside ASCII arrives as \uXXXX. The reader used to drop
 * the backslash and keep the rest: a file named in Greek showed as
 * "u0394u03af..." (PS1B-413), and \n became the letter n. This reader turns
 * the escapes into the text they stand for, as UTF-8, which is what the
 * launcher draws.
 *
 * Header-only and free of the host's other code, so a test can compile it.
 */
#ifndef PSX_JSON_TEXT_H
#define PSX_JSON_TEXT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int psx_json_hex4(const char* p, unsigned* out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10u;
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

/* The value of "key" in a compact row ("key":"value"), with its escapes
 * resolved, as UTF-8. A line break or a tab in the value becomes a space: the
 * setup window wraps the text itself. The value is cut at out_cap, never
 * inside a character this function wrote. Returns 1 when a value was read. */
static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(line, pattern);
    if (!p || out_cap == 0) return 0;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"') {
        unsigned cp;
        if (*p != '\\' || !p[1]) {
            if (i + 1 >= out_cap) break;
            out[i++] = *p++;
            continue;
        }
        if (p[1] == 'u' && psx_json_hex4(p + 2, &cp)) {
            p += 6;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                unsigned lo;
                if (p[0] == '\\' && p[1] == 'u' && psx_json_hex4(p + 2, &lo) &&
                    lo >= 0xDC00u && lo <= 0xDFFFu) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    p += 6;
                } else {
                    cp = '?';
                }
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                cp = '?';
            }
        } else {
            const char e = p[1];
            p += 2;
            cp = (e == 'n' || e == 'r' || e == 't' || e == 'b' || e == 'f')
                     ? (unsigned)' ' : (unsigned)(unsigned char)e;
        }
        if (cp < 0x20u) cp = ' ';
        const size_t n = cp < 0x80u ? 1 : cp < 0x800u ? 2 : cp < 0x10000u ? 3 : 4;
        if (i + n >= out_cap) break;
        if (n == 1) {
            out[i++] = (char)cp;
        } else if (n == 2) {
            out[i++] = (char)(0xC0u | (cp >> 6));
            out[i++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (n == 3) {
            out[i++] = (char)(0xE0u | (cp >> 12));
            out[i++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[i++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            out[i++] = (char)(0xF0u | (cp >> 18));
            out[i++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[i++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[i++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    out[i] = '\0';
    return i > 0;
}

#endif /* PSX_JSON_TEXT_H */
