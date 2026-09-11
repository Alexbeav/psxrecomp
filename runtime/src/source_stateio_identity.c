/* source_stateio_identity.c — v7 checkpoint identity dimensions.
 *
 * Its own TU so it can include <windows.h> for the executable path without
 * dragging that into headers shared with main.cpp. */
#include "source_stateio_identity.h"
#include "source_tas_stateio.h"   /* env digest + allow-list + hex */
#include "psx_sha256.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <stdlib.h>       /* _environ */
#  define PSX_ENVIRON ((const char *const *)_environ)
#else
#  include <unistd.h>
extern char **environ;
#  define PSX_ENVIRON ((const char *const *)environ)
#endif

int source_stateio_file_sha256(const char *path, char out[65]) {
    FILE *f;
    psx_sha256_ctx ctx;
    uint8_t dig[32], buf[65536];
    size_t n;
    if (!out) return 0;
    out[0] = '\0';
    if (!path || !*path) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    psx_sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) psx_sha256_update(&ctx, buf, n);
    if (ferror(f)) { fclose(f); return 0; }
    fclose(f);
    psx_sha256_final(&ctx, dig);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", (unsigned)dig[i]);
    out[64] = '\0';
    return 1;
}

int source_stateio_exe_sha256(char out[65]) {
    char path[1024];
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path) { if (out) out[0] = '\0'; return 0; }
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0 || (size_t)n >= sizeof path) { if (out) out[0] = '\0'; return 0; }
    path[n] = '\0';
#endif
    return source_stateio_file_sha256(path, out);
}

int source_stateio_config_digest_hex(char out[65]) {
    /* sha256 over the SORTED identity-relevant NAME=value list. Sorted so
     * enumeration order cannot change the result; a stream hash rather than an
     * XOR so two distinct configurations cannot cancel each other out. */
    const char *entries[512];
    size_t n;
    psx_sha256_ctx ctx;
    uint8_t dig[32];
    if (!out) return 0;
    out[0] = '\0';
    n = source_tas_stateio_env_collect(PSX_ENVIRON, entries, 512);
    psx_sha256_init(&ctx);
    for (size_t i = 0; i < n; i++) {
        psx_sha256_update(&ctx, (const uint8_t *)entries[i], strlen(entries[i]));
        psx_sha256_update(&ctx, (const uint8_t *)"\n", 1);
    }
    psx_sha256_final(&ctx, dig);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", (unsigned)dig[i]);
    out[64] = '\0';
    return 1;
}
