#include "replay_marks.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

int replay_marks_path(const char *replay, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s.marks.json", replay ? replay : "");
    return replay && replay[0] && n >= 0 && (size_t)n < cap;
}

void replay_marks_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* This bounded schema has only objects, an array, strings, integers and bools.
 * Parse those tokens strictly; do not reuse debug-server substring searches. */
static void space(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') ++*p;
}
static int token(const char **p, char c)
{
    space(p);
    if (**p != c) return 0;
    ++*p;
    return 1;
}
static int hex4(const char **p, unsigned *v)
{
    *v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)**p;
        unsigned n = c >= '0' && c <= '9' ? c - '0' :
                     c >= 'a' && c <= 'f' ? c - 'a' + 10u :
                     c >= 'A' && c <= 'F' ? c - 'A' + 10u : 16u;
        if (n == 16u) return 0;
        ++*p; *v = *v * 16u + n;
    }
    return 1;
}
static int string(const char **p, char *out, size_t cap)
{
    size_t n = 0;
    if (!token(p, '"')) return 0;
    while (**p && **p != '"') {
        unsigned c = (unsigned char)*(*p)++;
        if (c < 32) return 0;
        if (c == '\\') {
            c = (unsigned char)*(*p)++;
            if (c == 'u') {
                if (!hex4(p, &c)) return 0;
                if (c >= 0xd800 && c <= 0xdbff) {
                    unsigned lo;
                    if ((*p)[0] != '\\' || (*p)[1] != 'u') return 0;
                    *p += 2;
                    if (!hex4(p, &lo) ||
                        lo < 0xdc00 || lo > 0xdfff) return 0;
                    c = 0x10000u + ((c - 0xd800u) << 10) + lo - 0xdc00u;
                } else if (c >= 0xdc00 && c <= 0xdfff) return 0;
                if (c < 32) return 0; /* labels have no embedded control bytes */
                unsigned bytes = c < 0x80 ? 1u : c < 0x800 ? 2u : c < 0x10000 ? 3u : 4u;
                if (n + bytes >= cap) return 0;
                if (bytes == 1) out[n++] = (char)c;
                else {
                    out[n++] = (char)((bytes == 2 ? 0xc0u : bytes == 3 ? 0xe0u : 0xf0u) |
                                     (c >> (6 * (bytes - 1))));
                    for (unsigned k = bytes - 1; k; --k)
                        out[n++] = (char)(0x80u | ((c >> (6 * (k - 1))) & 63u));
                }
                continue;
            }
            if (c != '"' && c != '\\' && c != '/') return 0;
        }
        if (n + 1 >= cap) return 0;
        out[n++] = (char)c;
    }
    if (!token(p, '"')) return 0;
    out[n] = 0;
    return 1;
}
static int number(const char **p, uint32_t *out)
{
    uint32_t n = 0;
    space(p);
    if (**p < '0' || **p > '9') return 0;
    int zero = **p == '0';
    do {
        unsigned d = (unsigned)(*(*p)++ - '0');
        if (n > (UINT32_MAX - d) / 10u) return 0;
        n = n * 10u + d;
        if (zero && **p >= '0' && **p <= '9') return 0;
    } while (**p >= '0' && **p <= '9');
    *out = n;
    return 1;
}
static int label_valid(const char *label)
{
    size_t n = strlen(label);
    if (!n || n > REPLAY_MARK_LABEL_MAX) return 0;
    /* Validate UTF-8, including overlong sequences and surrogate code points. */
    const unsigned char *p = (const unsigned char *)label;
    while (*p) {
        unsigned c = *p++, need, value, minimum;
        if (c < 0x80) { if (c < 32 || c == 127) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { need = 1; value = c & 31u; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; value = c & 15u; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; value = c & 7u; minimum = 0x10000; }
        else return 0;
        for (unsigned i = 0; i < need; ++i) {
            c = *p++;
            if (c < 0x80 || c > 0xbf) return 0;
            value = value * 64u + (c & 63u);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0x80 && value <= 0x9f) ||
            (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}
int replay_marks_add(ReplayMarks *marks, uint32_t frame, const char *label, int estimated)
{
    unsigned i;
    if (!label_valid(label)) return 0;
    for (i = 0; i < marks->count; ++i) if (!strcmp(marks->items[i].label, label)) break;
    if (i == REPLAY_MARKS_MAX) return 0;
    if (i == marks->count) ++marks->count;
    memset(&marks->items[i], 0, sizeof marks->items[i]);
    marks->items[i].frame = frame;
    marks->items[i].estimated = estimated != 0;
    marks->items[i].in_sync = -1;
    strcpy(marks->items[i].label, label);
    return 1;
}
static int parse_mark(const char **p, ReplayMarks *marks, uint32_t total)
{
    char key[16], label[REPLAY_MARK_LABEL_MAX + 1] = "";
    uint32_t frame = 0;
    unsigned seen = 0;
    int estimated = 0;
    if (!token(p, '{')) return 0;
    do {
        if (!string(p, key, sizeof key) || !token(p, ':')) return 0;
        unsigned bit;
        if (!strcmp(key, "frame")) { bit = 1; if (!number(p, &frame)) return 0; }
        else if (!strcmp(key, "label")) { bit = 2; if (!string(p, label, sizeof label)) return 0; }
        else if (!strcmp(key, "estimated")) {
            bit = 4; space(p);
            if (!strncmp(*p, "true", 4)) { estimated = 1; *p += 4; }
            else if (!strncmp(*p, "false", 5)) *p += 5;
            else return 0;
        } else return 0;
        if (seen & bit) return 0;
        seen |= bit;
    } while (token(p, ','));
    if (!token(p, '}') || (seen & 3u) != 3u || frame > total || marks->count == REPLAY_MARKS_MAX) return 0;
    for (unsigned i = 0; i < marks->count; ++i) if (!strcmp(marks->items[i].label, label)) return 0;
    return replay_marks_add(marks, frame, label, estimated);
}
static int parse(const char *p, uint32_t total, ReplayMarks *marks)
{
    char key[16], schema[40];
    unsigned seen = 0;
    if (!token(&p, '{')) return 0;
    do {
        if (!string(&p, key, sizeof key) || !token(&p, ':')) return 0;
        unsigned bit;
        if (!strcmp(key, "schema")) {
            bit = 1;
            if (!string(&p, schema, sizeof schema) || strcmp(schema, "psxrecomp-replay-marks/1")) return 0;
        } else if (!strcmp(key, "marks")) {
            bit = 2;
            if (!token(&p, '[')) return 0;
            if (!token(&p, ']')) {
                do { if (!parse_mark(&p, marks, total)) return 0; } while (token(&p, ','));
                if (!token(&p, ']')) return 0;
            }
        } else return 0;
        if (seen & bit) return 0;
        seen |= bit;
    } while (token(&p, ','));
    if (!token(&p, '}') || seen != 3) return 0;
    space(&p);
    return !*p;
}
const char *replay_marks_read(const char *replay, uint32_t total, ReplayMarks *out)
{
    char path[1100];
    FILE *f;
    const char *error = NULL;
    memset(out, 0, sizeof *out);
    if (!replay_marks_path(replay, path, sizeof path)) return "mark file path too long";
    f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? NULL : "cannot read replay marks";
    char *data = (char *)malloc(REPLAY_MARKS_BYTES_MAX + 1u);
    if (!data) { fclose(f); return "out of memory for replay marks"; }
    size_t n = fread(data, 1, REPLAY_MARKS_BYTES_MAX, f);
    data[n] = 0;
    if (ferror(f) || fgetc(f) != EOF || memchr(data, 0, n) || !parse(data, total, out))
        error = "invalid replay marks";
    if (fclose(f)) error = "cannot read replay marks";
    free(data);
    if (error) memset(out, 0, sizeof *out);
    return error;
}
const char *replay_marks_save(const char *replay, const ReplayMarks *marks, FILE **owner)
{
    char path[1100];
    if (!replay_marks_path(replay, path, sizeof path)) return "mark file path too long";
    if (!*owner) {
#ifdef _WIN32
        int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        *owner = fd < 0 ? NULL : _fdopen(fd, "wb");
        if (!*owner && fd >= 0) _close(fd);
#else
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        *owner = fd < 0 ? NULL : fdopen(fd, "wb");
        if (!*owner && fd >= 0) close(fd);
#endif
        if (!*owner) return "cannot create new replay marks";
    }
    FILE *f = *owner;
#ifndef _WIN32
    /* An unlinked/replaced POSIX path must not be reported as a saved mark.
     * Even if a race replaces it later, writes still target our own inode. */
    struct stat opened, named;
    if (fstat(fileno(f), &opened) || stat(path, &named) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
        return "recording mark file was replaced";
#endif
    if (fflush(f) || fseek(f, 0, SEEK_SET)) return "cannot update recording marks";
#ifdef _WIN32
    if (_chsize(_fileno(f), 0)) return "cannot update recording marks";
#else
    if (ftruncate(fileno(f), 0)) return "cannot update recording marks";
#endif
    fputs("{\n  \"schema\": \"psxrecomp-replay-marks/1\",\n  \"marks\": [", f);
    for (unsigned i = 0; i < marks->count; ++i) {
        const ReplayMark *m = &marks->items[i];
        fprintf(f, "%s\n    {\"frame\": %u, \"label\": ", i ? "," : "", (unsigned)m->frame);
        replay_marks_json_string(f, m->label);
        fprintf(f, ", \"estimated\": %s}", m->estimated ? "true" : "false");
    }
    fputs("\n  ]\n}\n", f);
    if (ferror(f) || fflush(f)) return "cannot write replay marks";
#ifndef _WIN32
    if (stat(path, &named) || opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
        return "recording mark file was replaced";
#endif
    return NULL;
}
const char *replay_marks_write(const char *replay, const ReplayMarks *marks)
{
    FILE *owner = NULL;
    const char *error = replay_marks_save(replay, marks, &owner);
    if (owner && fclose(owner) && !error) error = "cannot close replay marks";
    return error;
}
int replay_marks_resolve(const ReplayMarks *marks, const char *selector,
                         uint32_t total, uint32_t *frame)
{
    if (!selector || !selector[0]) return 0;
    for (unsigned i = 0; i < marks->count; ++i)
        if (!strcmp(marks->items[i].label, selector)) { *frame = marks->items[i].frame; return *frame <= total; }
    const char *plus = strrchr(selector, '+');
    uint32_t offset;
    if (!plus || plus == selector) return 0;
    const char *p = plus + 1;
    if (!number(&p, &offset) || *p) return 0;
    for (unsigned i = 0; i < marks->count; ++i) {
        const ReplayMark *m = &marks->items[i];
        if (strlen(m->label) == (size_t)(plus - selector) && !strncmp(m->label, selector, (size_t)(plus - selector))) {
            if (m->frame > total || offset > total - m->frame) return 0;
            *frame = m->frame + offset;
            return 1;
        }
    }
    return 0;
}
