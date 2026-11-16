/* source_stateio_identity.c — v7 checkpoint identity dimensions.
 *
 * Its own TU so it can include <windows.h> for the executable path without
 * dragging that into headers shared with main.cpp. */
#include "source_stateio_identity.h"
#include "source_tas_stateio.h"   /* env digest + allow-list + hex */
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
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

/* The running executable and the route file cannot change during a run, and
 * hashing them is the slow part of a capture: a 94 MB title executable took
 * 22.7 s under a loaded host at Mega Man X4 return 25,000, and the starvation
 * watchdog aborted the replay. Each digest is computed once per process. */
int source_stateio_exe_sha256(char out[65]) {
    static char cached[65];
    char path[1024];
    if (!out) return 0;
    if (cached[0]) { memcpy(out, cached, 65); return 1; }
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path) { out[0] = '\0'; return 0; }
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0 || (size_t)n >= sizeof path) { out[0] = '\0'; return 0; }
    path[n] = '\0';
#endif
    if (!source_stateio_file_sha256(path, out)) return 0;
    memcpy(cached, out, 65);
    return 1;
}

int source_stateio_route_sha256(char out[65]) {
    static char cached[65];
    if (!out) return 0;
    if (cached[0]) { memcpy(out, cached, 65); return 1; }
    if (!source_stateio_file_sha256(getenv("PSX_INPUT_ROUTE_FILE"), out)) return 0;
    memcpy(cached, out, 65);
    return 1;
}

/* "content:<sha256>:<bytes>" for an input file named by a content-bound PSX_*
 * variable. An input file cannot change during a run, so the binding is kept
 * per process and a checkpoint does not rehash it. Returns 0 when the file
 * cannot be read; the caller then binds the value as written. */
int source_stateio_content_binding(const char *path, char *out, size_t cap) {
    static char cached_path[1024];
    static char cached[128];
    char hex[65];
    FILE *f;
    long long bytes;
    size_t len;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!path || !*path || strlen(path) >= sizeof cached_path) return 0;
    if (!cached[0] || strcmp(cached_path, path) != 0) {
        if (!source_stateio_file_sha256(path, hex)) return 0;
        f = fopen(path, "rb");
        if (!f) return 0;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
#if defined(_WIN32)
        bytes = (long long)_ftelli64(f);
#else
        bytes = (long long)ftello(f);
#endif
        fclose(f);
        if (bytes < 0) return 0;
        snprintf(cached, sizeof cached, "content:%s:%lld", hex, bytes);
        memcpy(cached_path, path, strlen(path) + 1);
    }
    len = strlen(cached);
    if (len + 1 > cap) return 0;
    memcpy(out, cached, len + 1);
    return 1;
}

/* sha256 over the SORTED identity-relevant NAME=value list. Sorted so
 * enumeration order cannot change the result; a stream hash rather than an XOR
 * so two distinct configurations cannot cancel each other out. With
 * content_bound, content-bound input paths enter as their content binding;
 * without it, as written (the form states saved before 2026-10-31 carry). */
static int config_digest_hex(char out[65], int content_bound, const char *legacy_clock_path, int omit_spu_keyoff) {
    const char *entries[512];
    size_t n;
    psx_sha256_ctx ctx;
    uint8_t dig[32];
    if (!out) return 0;
    out[0] = '\0';
    n = source_tas_stateio_env_collect(PSX_ENVIRON, entries, 512);
    psx_sha256_init(&ctx);
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(entries[i], '=');
        char bound[128];
        if (omit_spu_keyoff && eq && (size_t)(eq - entries[i]) == strlen("PSX_SPU_KEYOFF_MODEL") &&
            !strncmp(entries[i], "PSX_SPU_KEYOFF_MODEL", (size_t)(eq - entries[i]))) continue;
        if (legacy_clock_path && eq &&
            (size_t)(eq - entries[i]) == strlen("PSX_CD_SOURCE_CLOCK_TAPE") &&
            strncmp(entries[i], "PSX_CD_SOURCE_CLOCK_TAPE", (size_t)(eq - entries[i])) == 0) {
            psx_sha256_update(&ctx, (const uint8_t *)entries[i], (size_t)(eq - entries[i]) + 1);
            psx_sha256_update(&ctx, (const uint8_t *)legacy_clock_path, strlen(legacy_clock_path));
        } else if (content_bound && eq && eq[1] &&
            source_tas_stateio_env_content_bound(entries[i], (size_t)(eq - entries[i])) &&
            source_stateio_content_binding(eq + 1, bound, sizeof bound)) {
            /* NAME=content:<sha256>:<bytes>, never the path itself. */
            psx_sha256_update(&ctx, (const uint8_t *)entries[i], (size_t)(eq - entries[i]) + 1);
            psx_sha256_update(&ctx, (const uint8_t *)bound, strlen(bound));
        } else {
            psx_sha256_update(&ctx, (const uint8_t *)entries[i], strlen(entries[i]));
        }
        psx_sha256_update(&ctx, (const uint8_t *)"\n", 1);
    }
    psx_sha256_final(&ctx, dig);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", (unsigned)dig[i]);
    out[64] = '\0';
    return 1;
}

int source_stateio_config_digest_hex(char out[65]) {
    return config_digest_hex(out, 1, NULL, 0);
}

static int config_digest_legacy_hex(char out[65], int omit_spu_keyoff) {
    const char *old_path = getenv("PSX_TAS_RESUME_LEGACY_CLOCK_PATH");
    if (old_path && *old_path) {
        const char *expected = getenv("PSX_TAS_RESUME_LEGACY_CLOCK_SHA256");
        const char *current = getenv("PSX_CD_SOURCE_CLOCK_TAPE");
        char actual[65];
        /* Explicit migration reconstructs just one legacy path value. Every
         * other model value still enters the sorted digest. The copied tape
         * must match its recorded SHA; the native CD snapshot independently
         * validates its saved tape fingerprint/count before machine commit. */
        if (strchr(old_path, '\n') || strchr(old_path, '\r') || !expected ||
            strlen(expected) != 64 || !current || !*current ||
            !source_stateio_file_sha256(current, actual) || strcmp(actual, expected) != 0) {
            if (out) out[0] = '\0';
            return 0;
        }
        return config_digest_hex(out, 0, old_path, omit_spu_keyoff);
    }
    return config_digest_hex(out, 0, NULL, omit_spu_keyoff);
}

int source_stateio_config_digest_legacy_hex(char out[65]) {
    return config_digest_legacy_hex(out, 0);
}

int source_stateio_config_digest_spu_keyoff_legacy_hex(char out[65], int path_bound) {
    const char *from=getenv("PSX_TAS_RESUME_SPU_KEYOFF_FROM");
    const char *model=getenv("PSX_SPU_KEYOFF_MODEL");
    const char *compatible=getenv("PSX_TAS_RESUME_COMPATIBLE_BUILD");
    const char *state=getenv("PSX_TAS_RESUME_STATE");
    if (!from || strcmp(from,"unversioned") || !model || strcmp(model,"octoshock-1.11.9") ||
        !compatible || strcmp(compatible,"1") || !state || !*state) {
        if (out) out[0]='\0';
        return 0;
    }
    /* Reconstruct only the formerly absent model field. All other sorted
     * identity values, including the clock-content/path checks, stay bound.
     * No saved device data is modified by this interpretation transition. */
    return path_bound ? config_digest_legacy_hex(out, 1) : config_digest_hex(out, 1, NULL, 1);
}

int source_stateio_config_digest_accepts(const char *stored, int *legacy) {
    char now[65], old[65];
    if (legacy) *legacy=0;
    if (!stored || !source_stateio_config_digest_hex(now)) return 0;
    if (!strcmp(stored,now)) return 1;
    if (source_stateio_config_digest_legacy_hex(old) && !strcmp(stored,old)) {
        if (legacy) *legacy=1;
        return 1;
    }
    for (int path_bound=0;path_bound<2;path_bound++) {
        if (source_stateio_config_digest_spu_keyoff_legacy_hex(old,path_bound) && !strcmp(stored,old)) {
            if (legacy) *legacy=2+path_bound;
            return 1;
        }
    }
    return 0;
}
