/* Player replay identity extracted from the preserved route implementation. */
#include "replay_identity.h"
#include "disc_digest_cache.h"
#include "psx_sha256.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PSX_FRAMEWORK_PIN
#define PSX_FRAMEWORK_PIN ""
#endif
#define PATH_BYTES 4096u
static char s_product_serial[INPUT_ROUTE_V3_TEXT], s_product_disc[PATH_BYTES], s_product_bios_stem[INPUT_ROUTE_V3_TEXT];
static int s_digest_cached, s_replay_digest_cached;
static uint32_t s_digest_kind, s_replay_digest_kind;
static uint8_t s_digest[32], s_replay_digest[32];
static void hex(const uint8_t *bytes, unsigned n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = 0;
}

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= size) n = size - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

void replay_identity_set_product(const char *disc_serial,
                                     const char *disc_path,
                                     const char *bios_path)
{
    const char *base = bios_path ? bios_path : "";
    const char *dot;
    size_t n;
    copy_text(s_product_serial, sizeof(s_product_serial), disc_serial);
    copy_text(s_product_disc, sizeof(s_product_disc), disc_path);
    s_digest_cached = 0;
    s_replay_digest_cached = 0;
    for (const char *p = base; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof(s_product_bios_stem)) n = sizeof(s_product_bios_stem) - 1;
    memcpy(s_product_bios_stem, base, n);
    s_product_bios_stem[n] = 0;
}

void replay_identity_set_disc(const char *disc_serial, const char *disc_path)
{
    copy_text(s_product_serial, sizeof(s_product_serial), disc_serial);
    copy_text(s_product_disc, sizeof(s_product_disc), disc_path);
    s_digest_cached = 0;
    s_replay_digest_cached = 0;
}

static int ends_with_ci(const char *text, const char *suffix)
{
    size_t a = strlen(text), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; ++i)
        if (tolower((unsigned char)text[a - b + i]) != tolower((unsigned char)suffix[i]))
            return 0;
    return 1;
}

static int word_ci(const char *p, const char *word)
{
    for (; *word; ++p, ++word)
        if (toupper((unsigned char)*p) != *word) return 0;
    return 1;
}

/* One cue line. Returns 0 when not a FILE line, 1 with `name` filled for a
 * quoted BINARY FILE line, -1 for any other FILE line. */
static int cue_file_line(const char *line, char *name, size_t size)
{
    const char *p = line, *start;
    while (isspace((unsigned char)*p)) ++p;
    if (!word_ci(p, "FILE") || isalnum((unsigned char)p[4]) || p[4] == '_') return 0;
    p += 4;
    if (!isspace((unsigned char)*p)) return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '"') return -1;
    start = p;
    while (*p && *p != '"' && *p != '\r' && *p != '\n') ++p;
    if (*p != '"' || p == start || (size_t)(p - start) >= size) return -1;
    memcpy(name, start, (size_t)(p - start));
    name[p - start] = 0;
    ++p;
    if (!isspace((unsigned char)*p)) return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (!word_ci(p, "BINARY")) return -1;
    p += 6;
    while (isspace((unsigned char)*p)) ++p;
    return *p ? -1 : 1;
}

static const char *disc_digest_path(const char *path, uint32_t *kind,
                                    uint8_t out[32], int use_file_cache);

/* Route evidence (PSXRTI3 identity) always hashes the whole disc. Player
 * replays may use the persistent per-file cache (disc_digest_cache.h), which
 * trusts path, size, mtime and a head/tail spot hash. A full hash taken in
 * this process serves both (the s_replay_digest* cache is declared above). */

static const char *disc_digest(uint32_t *kind, uint8_t out[32])
{
    if (!s_digest_cached) {
        const char *error = disc_digest_path(s_product_disc, &s_digest_kind, s_digest, 0);
        if (error) return error;
        s_digest_cached = 1;
    }
    *kind = s_digest_kind;
    memcpy(out, s_digest, 32);
    return NULL;
}

static const char *replay_disc_digest(uint32_t *kind, uint8_t out[32])
{
    if (s_digest_cached) return disc_digest(kind, out);
    if (!s_replay_digest_cached) {
        const char *error = disc_digest_path(s_product_disc, &s_replay_digest_kind,
                                             s_replay_digest, 1);
        if (error) return error;
        s_replay_digest_cached = 1;
    }
    *kind = s_replay_digest_kind;
    memcpy(out, s_replay_digest, 32);
    return NULL;
}

int replay_identity_prefetch_disc_digest(void)
{
    uint32_t kind;
    uint8_t digest[32];
    return replay_disc_digest(&kind, digest) == NULL;
}

static const char *disc_digest_path(const char *path, uint32_t *kind,
                                    uint8_t out[32], int use_file_cache)
{
    int (*sha256_file)(const char *, uint8_t[32]) =
        use_file_cache ? disc_digest_cache_sha256 : disc_digest_full_sha256;
    uint8_t digest[32];
    char text[65];
    if (!path || !path[0]) return "no disc is mounted";
    if (!sha256_file(path, digest)) return "cannot read the disc image";
    if (!ends_with_ci(path, ".cue")) {
        *kind = INPUT_ROUTE_DISC_DIGEST_FILE;
        memcpy(out, digest, 32);
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    char *cue;
    long size;
    if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || size > (1L << 20) ||
        fseek(f, 0, SEEK_SET)) { if (f) fclose(f); return "cannot read the cue sheet"; }
    cue = (char *)malloc((size_t)size + 1);
    if (!cue || fread(cue, 1, (size_t)size, f) != (size_t)size) {
        free(cue); fclose(f); return "cannot read the cue sheet";
    }
    fclose(f);
    cue[size] = 0;
    psx_sha256_ctx ctx;
    const char *error = NULL;
    int tracks = 0;
    char dir[PATH_BYTES];
    size_t dir_len = 0;
    for (size_t i = 0; path[i]; ++i)
        if (path[i] == '/' || path[i] == '\\') dir_len = i + 1;
    memcpy(dir, path, dir_len);
    psx_sha256_init(&ctx);
    hex(digest, 32, text);
    psx_sha256_update(&ctx, (const uint8_t *)text, 64);
    char *line = cue;
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) line += 3;
    while (!error && line && *line) {
        char *next = strpbrk(line, "\r\n");
        char name[PATH_BYTES], track[PATH_BYTES];
        if (next) *next++ = 0;
        int kind_line = cue_file_line(line, name, sizeof(name));
        if (kind_line < 0) error = "cue FILE line is not a quoted BINARY track";
        else if (kind_line > 0) {
            if (dir_len + strlen(name) >= sizeof(track)) error = "cue track path too long";
            else {
                memcpy(track, dir, dir_len);
                strcpy(track + dir_len, name);
                if (!sha256_file(track, digest)) error = "cannot read a cue track";
                else {
                    hex(digest, 32, text);
                    psx_sha256_update(&ctx, (const uint8_t *)"\n", 1);
                    psx_sha256_update(&ctx, (const uint8_t *)text, 64);
                    ++tracks;
                }
            }
        }
        line = next;
    }
    free(cue);
    if (error) return error;
    if (!tracks) return "cue sheet has no tracks";
    psx_sha256_final(&ctx, out);
    *kind = INPUT_ROUTE_DISC_DIGEST_CUE;
    return NULL;
}

/* ---- Checkpoints ---- */

static const char *boot_mode_name(int call_hle, int boot_skip)
{
    if (call_hle && boot_skip) return "hle";
    if (call_hle) return "hle-calls";
    if (boot_skip) return "hle-boot";
    return "lle";
}

static int valid_pin(const char *pin)
{
    if (strlen(pin) != 40) return 0;
    for (unsigned i = 0; i < 40; ++i)
        if (!((pin[i] >= '0' && pin[i] <= '9') || (pin[i] >= 'a' && pin[i] <= 'f')))
            return 0;
    return 1;
}

int replay_identity_identity(InputRouteV3 *meta, int call_hle, int boot_skip,
                                 char *why, size_t why_size)
{
    const char *pin = PSX_FRAMEWORK_PIN;
    uint32_t kind = 0;
    uint8_t digest[32];
    const char *error = NULL;
    if (!s_product_serial[0] || !s_product_bios_stem[0])
        error = "a disc with a boot serial and a BIOS file are required";
    else
        error = replay_disc_digest(&kind, digest);   /* player replays only */
    if (error) { snprintf(why, why_size, "%s", error); return 0; }
    meta->has_identity = 1;
    /* A product built outside a git checkout has no pin; replays still carry
     * a (non-matching) placeholder so playback can warn. */
    copy_text(meta->pin, sizeof(meta->pin),
              valid_pin(pin) ? pin : "0000000000000000000000000000000000000000");
    copy_text(meta->disc_serial, sizeof(meta->disc_serial), s_product_serial);
    copy_text(meta->bios_stem, sizeof(meta->bios_stem), s_product_bios_stem);
    copy_text(meta->boot_mode, sizeof(meta->boot_mode), boot_mode_name(call_hle, boot_skip));
    meta->disc_digest_kind = kind;
    memcpy(meta->disc_digest, digest, 32);
    return 1;
}

