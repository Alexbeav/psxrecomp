/* Disc digest cache (PS1B-191). See disc_digest_cache.h. */
#include "disc_digest_cache.h"
#include "psx_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE_PATH_BYTES 1024

static char s_cache_path[CACHE_PATH_BYTES];

static volatile int s_cancel;   /* set from the shutdown thread; read per chunk */

void disc_digest_cache_cancel(int cancel)
{
    s_cancel = cancel ? 1 : 0;
}

void disc_digest_cache_set_path(const char *cache_file)
{
    snprintf(s_cache_path, sizeof s_cache_path, "%s", cache_file ? cache_file : "");
}

static int file_key(const char *path, unsigned long long *size, long long *mtime)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#endif
    *size = (unsigned long long)st.st_size;
    *mtime = (long long)st.st_mtime;
    return 1;
}

static int sha256_file(const char *path, uint8_t out[32])
{
    enum { CHUNK = 1 << 20 };
    psx_sha256_ctx ctx;
    unsigned char *buffer;
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    buffer = (unsigned char *)malloc(CHUNK);
    if (!buffer) { fclose(f); return 0; }
    psx_sha256_init(&ctx);
    while (!s_cancel && (n = fread(buffer, 1, CHUNK, f)) > 0) psx_sha256_update(&ctx, buffer, n);
    n = (size_t)ferror(f);
    free(buffer);
    if (fclose(f) || n || s_cancel) return 0;
    psx_sha256_final(&ctx, out);
    return 1;
}

/* SHA-256 of the first and last 64 KiB, first 8 bytes as hex: a cheap guard
 * against a same-size rewrite that kept its mtime. */
static int spot_hex(const char *path, unsigned long long size, char out[17])
{
    enum { SPAN = 64 * 1024 };
    static const char digits[] = "0123456789abcdef";
    unsigned char *buf = (unsigned char *)malloc(SPAN);
    psx_sha256_ctx ctx;
    uint8_t d[32];
    size_t n;
    int ok;
    FILE *f = fopen(path, "rb");
    if (!f || !buf) { free(buf); if (f) fclose(f); return 0; }
    psx_sha256_init(&ctx);
    n = fread(buf, 1, SPAN, f);
    psx_sha256_update(&ctx, buf, n);
    ok = !ferror(f);
    if (ok && size > SPAN) {
#ifdef _WIN32
        ok = _fseeki64(f, (long long)(size - SPAN), SEEK_SET) == 0;
#else
        ok = fseeko(f, (off_t)(size - SPAN), SEEK_SET) == 0;
#endif
        n = ok ? fread(buf, 1, SPAN, f) : 0;
        psx_sha256_update(&ctx, buf, n);
        ok = ok && !ferror(f);
    }
    fclose(f);
    free(buf);
    if (!ok) return 0;
    psx_sha256_final(&ctx, d);
    for (int i = 0; i < 8; ++i) { out[2 * i] = digits[d[i] >> 4]; out[2 * i + 1] = digits[d[i] & 15]; }
    out[16] = 0;
    return 1;
}

static int unhex(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 32; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = s[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else return 0;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

/* Two spellings of one path. On Windows a path arrives with either separator
 * ("Z:\discs\a.chd" from a file dialog, "Z:/discs/a.chd" from settings.toml),
 * and both name the same file. Compared as text they were two cache lines, so
 * the whole image was hashed again for the second spelling. Elsewhere a
 * backslash is an ordinary character of a file name, and the text decides. */
static int same_path(const char *a, const char *b)
{
#ifdef _WIN32
    for (;; ++a, ++b) {
        const char ca = *a == '\\' ? '/' : *a;
        const char cb = *b == '\\' ? '/' : *b;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
#else
    return strcmp(a, b) == 0;
#endif
}

/* The last line for this path, size, mtime and head/tail spot hash. */
static int cache_lookup(const char *path, unsigned long long size, long long mtime,
                        const char *spot, uint8_t out[32])
{
    char line[CACHE_PATH_BYTES + 128];
    int found = 0;
    FILE *f = fopen(s_cache_path, "rb");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lsize;
        long long lmtime;
        char hex[65], lspot[17];
        int used = 0;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (sscanf(line, "%llu\t%lld\t%16[0-9a-f]\t%64[0-9a-f]\t%n",
                   &lsize, &lmtime, lspot, hex, &used) != 4 || !used)
            continue;
        if (lsize == size && lmtime == mtime && !strcmp(lspot, spot) && strlen(hex) == 64 &&
            same_path(line + used, path) && unhex(hex, out))
            found = 1;
    }
    fclose(f);
    return found;
}

int disc_digest_full_sha256(const char *path, uint8_t out[32])
{
    return path && path[0] && sha256_file(path, out);
}

int disc_digest_cache_sha256(const char *path, uint8_t out[32])
{
    unsigned long long size;
    long long mtime;
    if (!path || !path[0]) return 0;
    if (!s_cache_path[0]) return sha256_file(path, out);
    char spot[17];
    if (!file_key(path, &size, &mtime) || !spot_hex(path, size, spot)) return 0;
    if (cache_lookup(path, size, mtime, spot, out)) return 1;
    if (!sha256_file(path, out)) return 0;
    FILE *f = fopen(s_cache_path, "ab");
    if (f) {
        fprintf(f, "%llu\t%lld\t%s\t", size, mtime, spot);
        for (int i = 0; i < 32; ++i) fprintf(f, "%02x", out[i]);
        fprintf(f, "\t%s\n", path);
        fclose(f);
    }
    return 1;
}
