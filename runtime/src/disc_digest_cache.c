/* Disc digest cache (PS1B-191). See disc_digest_cache.h. */
#include "disc_digest_cache.h"
#include "psx_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE_PATH_BYTES 1024

static char s_cache_path[CACHE_PATH_BYTES];

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
    while ((n = fread(buffer, 1, CHUNK, f)) > 0) psx_sha256_update(&ctx, buffer, n);
    n = (size_t)ferror(f);
    free(buffer);
    if (fclose(f) || n) return 0;
    psx_sha256_final(&ctx, out);
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

/* The last line for this exact path, size and mtime. */
static int cache_lookup(const char *path, unsigned long long size, long long mtime, uint8_t out[32])
{
    char line[CACHE_PATH_BYTES + 128];
    int found = 0;
    FILE *f = fopen(s_cache_path, "rb");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lsize;
        long long lmtime;
        char hex[65];
        int used = 0;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (sscanf(line, "%llu\t%lld\t%64[0-9a-f]\t%n", &lsize, &lmtime, hex, &used) != 3 || !used)
            continue;
        if (lsize == size && lmtime == mtime && strlen(hex) == 64 && !strcmp(line + used, path) &&
            unhex(hex, out))
            found = 1;
    }
    fclose(f);
    return found;
}

int disc_digest_cache_sha256(const char *path, uint8_t out[32])
{
    unsigned long long size;
    long long mtime;
    if (!path || !path[0]) return 0;
    if (!s_cache_path[0]) return sha256_file(path, out);
    if (!file_key(path, &size, &mtime)) return 0;
    if (cache_lookup(path, size, mtime, out)) return 1;
    if (!sha256_file(path, out)) return 0;
    FILE *f = fopen(s_cache_path, "ab");
    if (f) {
        fprintf(f, "%llu\t%lld\t", size, mtime);
        for (int i = 0; i < 32; ++i) fprintf(f, "%02x", out[i]);
        fprintf(f, "\t%s\n", path);
        fclose(f);
    }
    return 1;
}
