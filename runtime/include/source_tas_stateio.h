#ifndef PSX_SOURCE_TAS_STATEIO_H
#define PSX_SOURCE_TAS_STATEIO_H

/*
 * TAS checkpoint identity binding (Bio Hazard accuracy loop).
 *
 * A native checkpoint is only usable for the diagnostic loop if it can be
 * PROVEN to belong to the exact build, BIOS, entry point, return index and
 * guest state it claims. This header owns that proof: a small JSON manifest
 * written beside the .pst state plus a pure accept/reject gate. The runtime
 * glue (boot_state save/load) lives at the call sites, so this file stays
 * dependency-light and unit-testable.
 *
 * Env contract (read by the call sites, not here):
 *   PSX_TAS_SAVE_STATE_AT=<return>       save once at this frontend return
 *   PSX_TAS_SAVE_STATE_PATH=<file.pst>   state path
 *   PSX_TAS_RESUME_STATE=<file.pst>      restore this state during startup
 *   PSX_TAS_RESUME_MANIFEST=<file.json>  its manifest (default state path + ".json")
 *
 * Trust nothing: a resumed run is only admitted when the restored state
 * reproduces the manifest's frame, cycle and RAM digest AND the manifest was
 * produced by the same BIOS/entry identity. Rejections carry a reason.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSX_TAS_STATEIO_SCHEMA "psx-tas-stateio-v1"

/* Guest RAM accessor (memory.c). Declared here so the manifest/digest call
 * sites — including C++ — share one C-linkage declaration. */
#ifdef __cplusplus
extern "C" {
#endif
uint8_t *memory_get_ram_ptr(void);
#ifdef __cplusplus
}
#endif

typedef struct TasStateManifest {
    unsigned           frame;
    uint64_t           cycle;
    uint64_t           ram_digest;
    uint32_t           bios_checksum;
    uint32_t           entry_pc;
    unsigned long long state_bytes;
} TasStateManifest;

/* FNV-1a over guest RAM — same family as the RAM page probe, cheap enough to
 * run once per checkpoint. */
static inline uint64_t source_tas_stateio_ram_digest(const uint8_t *ram, size_t bytes) {
    uint64_t h = UINT64_C(14695981039346656037);
    if (!ram) return 0;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ (uint64_t)ram[i]) * UINT64_C(1099511628211);
    return h;
}

static inline int source_tas_stateio_manifest_write(const char *path, const TasStateManifest *m,
                                             const char *state_path, const char *state_sha256) {
    FILE *f;
    int ok;
    if (!path || !m || !state_path) return 0;
    f = fopen(path, "wbx");
    if (!f) return 0;
    ok = fprintf(f,
                 "{\n"
                 "  \"schema\": \"%s\",\n"
                 "  \"frame\": %u,\n"
                 "  \"cycle\": %llu,\n"
                 "  \"ram_digest\": \"%016llX\",\n"
                 "  \"bios_checksum\": %u,\n"
                 "  \"entry_pc\": %u,\n"
                 "  \"state_path\": \"%s\",\n"
                 "  \"state_sha256\": \"%s\",\n"
                 "  \"state_bytes\": %llu\n"
                 "}\n",
                 PSX_TAS_STATEIO_SCHEMA, m->frame, (unsigned long long)m->cycle,
                 (unsigned long long)m->ram_digest, m->bios_checksum, m->entry_pc,
                 state_path, state_sha256 ? state_sha256 : "", m->state_bytes) > 0;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* Extract a JSON scalar by key. Values may be quoted strings or bare numbers.
 * Deliberately minimal (strstr-based) so it has no parser dependency; the
 * manifest is written by this same module, not user-authored. */
static inline int source_tas_stateio_find_field(const char *text, const char *key,
                                         char *out, size_t cap) {
    char needle[64];
    const char *p;
    size_t n = 0;
    if (!text || !key || !out || cap == 0) return 0;
    if (snprintf(needle, sizeof needle, "\"%s\"", key) >= (int)sizeof needle) return 0;
    p = strstr(text, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != ':') return 0;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && *p != '\n' && n + 1 < cap) out[n++] = *p++;
    } else {
        while (*p && *p != ',' && *p != '\n' && *p != '}' && *p != ' ' && n + 1 < cap)
            out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

static inline int source_tas_stateio_parse_u64(const char *text, const char *key, uint64_t *out) {
    char buf[64];
    char *end;
    unsigned long long v;
    if (!source_tas_stateio_find_field(text, key, buf, sizeof buf)) return 0;
    v = strtoull(buf, &end, 10);
    if (end == buf || *end) return 0;
    *out = (uint64_t)v;
    return 1;
}

/* ram_digest is written as 16 hex digits; parse it base 16. */
static inline int source_tas_stateio_parse_digest(const char *text, uint64_t *out) {
    char buf[64];
    char *end;
    unsigned long long v;
    if (!source_tas_stateio_find_field(text, "ram_digest", buf, sizeof buf)) return 0;
    v = strtoull(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = (uint64_t)v;
    return 1;
}

/* Parse a manifest. Returns 1 only for a complete, schema-matching record. */
static inline int source_tas_stateio_manifest_parse(const char *text, TasStateManifest *out,
                                             char *state_path, size_t state_path_cap) {
    char schema[64];
    uint64_t v;
    if (!text || !out) return 0;
    memset(out, 0, sizeof *out);
    if (state_path && state_path_cap) state_path[0] = '\0';
    if (!source_tas_stateio_find_field(text, "schema", schema, sizeof schema)) return 0;
    if (strcmp(schema, PSX_TAS_STATEIO_SCHEMA) != 0) return 0;
    if (!source_tas_stateio_parse_u64(text, "frame", &v) || v > 0xFFFFFFFFull) return 0;
    out->frame = (unsigned)v;
    if (!source_tas_stateio_parse_u64(text, "cycle", &out->cycle)) return 0;
    if (!source_tas_stateio_parse_digest(text, &out->ram_digest)) return 0;
    if (!source_tas_stateio_parse_u64(text, "bios_checksum", &v) || v > 0xFFFFFFFFull) return 0;
    out->bios_checksum = (uint32_t)v;
    if (!source_tas_stateio_parse_u64(text, "entry_pc", &v) || v > 0xFFFFFFFFull) return 0;
    out->entry_pc = (uint32_t)v;
    if (!source_tas_stateio_parse_u64(text, "state_bytes", &out->state_bytes)) return 0;
    if (state_path && state_path_cap) {
        if (!source_tas_stateio_find_field(text, "state_path", state_path, state_path_cap))
            return 0;
    }
    return 1;
}

static inline void source_tas_stateio_reject(char *reason, size_t cap, const char *text) {
    if (reason && cap) snprintf(reason, cap, "%s", text ? text : "rejected");
}

/* The gate. Returns 1 on accept, else 0 with a reason. The observed_* values
 * are what the resumed run actually reproduced. */
static inline int source_tas_stateio_manifest_accept(const TasStateManifest *m,
                                              unsigned frame, uint64_t cycle,
                                              uint64_t ram_digest, uint32_t bios_checksum,
                                              uint32_t entry_pc,
                                              char *reason, size_t reason_cap) {
    if (!m) { source_tas_stateio_reject(reason, reason_cap, "missing manifest"); return 0; }
    if (m->frame != frame) {
        source_tas_stateio_reject(reason, reason_cap, "frame mismatch");
        return 0;
    }
    if (m->cycle != cycle) {
        source_tas_stateio_reject(reason, reason_cap, "cycle mismatch");
        return 0;
    }
    if (m->ram_digest != ram_digest) {
        source_tas_stateio_reject(reason, reason_cap, "restored RAM digest mismatch");
        return 0;
    }
    if (m->bios_checksum != bios_checksum) {
        source_tas_stateio_reject(reason, reason_cap, "foreign BIOS state");
        return 0;
    }
    if (m->entry_pc != entry_pc) {
        source_tas_stateio_reject(reason, reason_cap, "foreign entry point");
        return 0;
    }
    if (reason && reason_cap) reason[0] = '\0';
    return 1;
}

/* Read a whole small file (manifest) into caller storage. Returns bytes read
 * or 0 on failure. */
static inline size_t source_tas_stateio_read_text(const char *path, char *out, size_t cap) {
    FILE *f;
    size_t n;
    if (!path || !out || cap < 2) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(out, 1, cap - 1, f);
    if (ferror(f)) n = 0;
    fclose(f);
    out[n] = '\0';
    return n;
}

/* Parse the save-at return from the environment. 0 = disabled. */
static inline unsigned source_tas_stateio_save_at(void) {
    const char *e = getenv("PSX_TAS_SAVE_STATE_AT");
    char *end;
    unsigned long v;
    if (!e || !*e) return 0;
    v = strtoul(e, &end, 10);
    if (end == e || *end || v == 0 || v > 0xFFFFFFFFul) return 0;
    return (unsigned)v;
}

#endif /* PSX_SOURCE_TAS_STATEIO_H */
