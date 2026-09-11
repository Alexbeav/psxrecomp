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
    /* v7 identity: the whole resolved configuration, not a hand-written flag
     * list, plus the two things a flag list can never cover — the exact runtime
     * binary and the exact route content. */
    char               config_digest[65];
    char               exe_sha256[65];
    char               route_sha256[65];
} TasStateManifest;

/* PSX_* variables that may legitimately DIFFER between a save and a resume.
 * Everything else the runtime reads is part of the identity, so a newly added
 * model flag is covered automatically instead of being forgotten. */
static inline int source_tas_stateio_env_allowed_to_differ(const char *key, size_t klen) {
    static const char *const allow[] = {
        "PSX_INPUT_ROUTE_CAPTURE_DIR",  /* output location            */
        "PSX_INPUT_ROUTE_FILE",         /* path only; CONTENT is hashed
                                           separately as route_sha256, because
                                           each run dir holds its own copy */
        "PSX_TAS_SAVE_STATE_AT",        /* checkpoint request         */
        "PSX_TAS_SAVE_STATE_PATH",      /* checkpoint request         */
        "PSX_TAS_RESUME_STATE",         /* resume request             */
        "PSX_TAS_RESUME_MANIFEST",      /* resume request             */
        "PSX_LOAD_SLOT",                /* user slot load request     */
        "PSX_E_SURVEY",                 /* diagnostic only            */
        "PSX_TAS_PERTURB_RESTORE",      /* negative-control injection */
        NULL
    };
    for (int i = 0; allow[i]; i++)
        if (strlen(allow[i]) == klen && strncmp(key, allow[i], klen) == 0) return 1;
    return 0;
}

/* FNV-1a over guest RAM — same family as the RAM page probe, cheap enough to
 * run once per checkpoint. */
static inline uint64_t source_tas_stateio_ram_digest(const uint8_t *ram, size_t bytes) {
    uint64_t h = UINT64_C(14695981039346656037);
    if (!ram) return 0;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ (uint64_t)ram[i]) * UINT64_C(1099511628211);
    return h;
}

/* Order-independent digest over the PSX_* entries of a NULL-terminated
 * "KEY=VALUE" environment array. Entries are SORTED first and then hashed as one
 * stream, so enumeration order cannot change the result and (unlike a
 * XOR-of-hashes) distinct configurations cannot cancel each other out. */
static inline uint64_t source_tas_stateio_env_digest(const char *const *env) {
    uint64_t acc = 0;
    if (!env) return 0;
    for (size_t i = 0; env[i]; i++) {
        const char *e = env[i];
        const char *eq = strchr(e, '=');
        size_t klen;
        if (!eq) continue;
        klen = (size_t)(eq - e);
        if (klen < 4 || strncmp(e, "PSX_", 4) != 0) continue;
        if (source_tas_stateio_env_allowed_to_differ(e, klen)) continue;
        acc ^= source_tas_stateio_ram_digest((const uint8_t *)e, strlen(e));
    }
    return acc;
}

/* Collect the identity-relevant PSX_* entries into `out` (up to `cap`), sorted by
 * byte order. Returns the count. Used for the sha256 configuration digest. */
static inline size_t source_tas_stateio_env_collect(const char *const *env,
                                                   const char **out, size_t cap) {
    size_t n = 0;
    if (!env) return 0;
    for (size_t i = 0; env[i] && n < cap; i++) {
        const char *e = env[i];
        const char *eq = strchr(e, '=');
        size_t klen;
        if (!eq) continue;
        klen = (size_t)(eq - e);
        if (klen < 4 || strncmp(e, "PSX_", 4) != 0) continue;
        if (source_tas_stateio_env_allowed_to_differ(e, klen)) continue;
        out[n++] = e;
    }
    for (size_t a = 1; a < n; a++) {          /* insertion sort; n is tiny */
        const char *v = out[a];
        size_t b = a;
        while (b > 0 && strcmp(out[b - 1], v) > 0) { out[b] = out[b - 1]; --b; }
        out[b] = v;
    }
    return n;
}
static inline void source_tas_stateio_hex64(uint64_t v, char out[17]) {
    snprintf(out, 17, "%016llX", (unsigned long long)v);
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
                 "  \"state_bytes\": %llu,\n"
                 "  \"config_digest\": \"%s\",\n"
                 "  \"exe_sha256\": \"%s\",\n"
                 "  \"route_sha256\": \"%s\"\n"
                 "}\n",
                 PSX_TAS_STATEIO_SCHEMA, m->frame, (unsigned long long)m->cycle,
                 (unsigned long long)m->ram_digest, m->bios_checksum, m->entry_pc,
                 state_path, state_sha256 ? state_sha256 : "", m->state_bytes,
                 m->config_digest, m->exe_sha256, m->route_sha256) > 0;
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
    if (!source_tas_stateio_find_field(text, "config_digest", out->config_digest,
                                       sizeof out->config_digest)) return 0;
    if (!source_tas_stateio_find_field(text, "exe_sha256", out->exe_sha256,
                                       sizeof out->exe_sha256)) return 0;
    if (!source_tas_stateio_find_field(text, "route_sha256", out->route_sha256,
                                       sizeof out->route_sha256)) return 0;
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
                                              const char *config_digest,
                                              const char *exe_sha256,
                                              const char *route_sha256,
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
    if (strcmp(m->config_digest, config_digest ? config_digest : "") != 0) {
        source_tas_stateio_reject(reason, reason_cap, "configuration digest mismatch");
        return 0;
    }
    if (strcmp(m->exe_sha256, exe_sha256 ? exe_sha256 : "") != 0) {
        source_tas_stateio_reject(reason, reason_cap, "runtime binary mismatch");
        return 0;
    }
    if (strcmp(m->route_sha256, route_sha256 ? route_sha256 : "") != 0) {
        source_tas_stateio_reject(reason, reason_cap, "input route mismatch");
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

/* Parse the save-at return list from the environment. Comma-separated so the
 * E test ladder can checkpoint at K and K+1 in one from-scratch run. An empty,
 * malformed or zero entry list is disabled. */
static inline int source_tas_stateio_save_at_match(unsigned frame) {
    const char *e = getenv("PSX_TAS_SAVE_STATE_AT");
    if (!e || !*e || frame == 0) return 0;
    while (*e) {
        char *end;
        unsigned long v = strtoul(e, &end, 10);
        if (end == e) return 0;
        if (v == (unsigned long)frame) return 1;
        if (*end != ',') return 0;
        e = end + 1;
    }
    return 0;
}

#endif /* PSX_SOURCE_TAS_STATEIO_H */
