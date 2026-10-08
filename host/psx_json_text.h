/* psx_json_text.h -- read one string value out of a compact JSON row.
 *
 * The setup host reads the CLI's progress rows ({"event":"error","message":
 * "..."}) and shows the message in the setup window.
 *
 * Header-only and free of the host's other code, so a test can compile it.
 */
#ifndef PSX_JSON_TEXT_H
#define PSX_JSON_TEXT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

#endif /* PSX_JSON_TEXT_H */
