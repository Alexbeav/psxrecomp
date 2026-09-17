#ifndef PSX_INPUT_ROUTE_V3_FILE_H
#define PSX_INPUT_ROUTE_V3_FILE_H

/* PSXRTI3: PSXRTI1/PSXRTI2 records behind a tagged extension block. PSXRTI1
 * and PSXRTI2 stay byte-frozen; a writer emits v3 only when it embeds
 * identity or markers. Agreed with the TAS lane (T101 / T98), 2026-09-17.
 *
 * Wire layout, little-endian:
 *   header   u8[8] "PSXRTI3\0", u32 version=3, u32 record_size,
 *            u32 frame_count (1..INPUT_ROUTE_MAX_FRAMES), u32 flags=0,
 *            u32 ext_bytes (multiple of 4, <= 16 MiB)
 *   entries  ext_bytes of: u32 tag, u32 length, payload[length],
 *            zero padding to 4 bytes (not counted in length)
 *   records  frame_count records of record_size, then end of file
 * The file size must equal 28 + ext_bytes + frame_count * record_size.
 *
 * Tag bit 31 marks an entry that does not change replay. A reader skips
 * unknown bit-31 tags and refuses any other unknown tag.
 *   0x800001xx  T101 identity and markers (this reader). Unknown 0x800001xx
 *               tags are refused so an identity field is never ignored.
 *   0x000002xx  T98 port layout, console events, disc set. Not implemented
 *               here, so refused as unknown mandatory tags.
 * Without a port-layout tag, record_size 8 is a PSXRTI1 record and 12 a
 * PSXRTI2 record, under the same strict record rules.
 *
 * Frame N in a marker or checkpoint is the boundary after record N was
 * supplied and before record N+1; frame 0 is before the first record.
 * Parsing touches no guest state and no live input. */
#include "input_route_file.h"
#include "input_dualshock_route_file.h"
#include <stdlib.h>

#define INPUT_ROUTE_V3_HEADER_BYTES 28u
#define INPUT_ROUTE_V3_MAX_EXT (16u << 20)
#define INPUT_ROUTE_V3_TEXT 128u
#define INPUT_ROUTE_V3_MAX_MARKERS 256u
#define INPUT_ROUTE_RAM_PAGES 512u /* 2 MiB main RAM, 4 KiB pages */

#define INPUT_ROUTE_TAG_SKIPPABLE   0x80000000u
#define INPUT_ROUTE_TAG_PIN         0x80000101u /* 40 lowercase hex framework commit */
#define INPUT_ROUTE_TAG_DISC_SERIAL 0x80000102u /* boot serial, e.g. SLUS-00662 */
#define INPUT_ROUTE_TAG_DISC_DIGEST 0x80000103u /* u8 kind, u8[3] 0, u8[32] SHA-256 */
#define INPUT_ROUTE_TAG_BIOS_STEM   0x80000104u /* BIOS file stem */
/* Boot mode names both BIOS HLE axes: "lle" (neither), "hle" (kernel calls
 * and boot skip), "hle-calls" (calls only), "hle-boot" (boot skip only). */
#define INPUT_ROUTE_TAG_BOOT_MODE   0x80000105u
#define INPUT_ROUTE_TAG_MARKER      0x80000110u /* u32 frame, u8 kind, u8[3] 0 */
#define INPUT_ROUTE_TAG_CHECKPOINT  0x80000111u /* see INPUT_ROUTE_CHECKPOINT_BYTES */

/* Disc digest kinds. CUE: SHA-256 over the ASCII hex digests of the cue file
 * and then each FILE track in cue order, joined by '\n', no trailing newline
 * (run_native.checkpoint_asset_digest). FILE: SHA-256 of the whole image. */
#define INPUT_ROUTE_DISC_DIGEST_CUE  1u
#define INPUT_ROUTE_DISC_DIGEST_FILE 2u

#define INPUT_ROUTE_MARKER_MENU     1u
#define INPUT_ROUTE_MARKER_GAMEPLAY 2u
#define INPUT_ROUTE_MARKER_BYTES    8u
/* u32 frame, u32 0, u64 guest cycle, u8[32] SHA-256 of main RAM,
 * u64[512] FNV-1a 64 hash per 4 KiB page (compare_ram_pages.py). */
#define INPUT_ROUTE_CHECKPOINT_BYTES (4u + 4u + 8u + 32u + 8u * INPUT_ROUTE_RAM_PAGES)

typedef struct {
    uint32_t frame;
    uint32_t kind;
} InputRouteMarker;

typedef struct {
    uint32_t frame;
    uint64_t cycle;
    uint8_t ram_sha256[32];
    uint64_t pages[INPUT_ROUTE_RAM_PAGES];
} InputRouteCheckpoint;

typedef struct {
    uint32_t record_size;
    uint32_t frames;
    uint32_t steps;
    int has_identity;
    char pin[INPUT_ROUTE_V3_TEXT];
    char disc_serial[INPUT_ROUTE_V3_TEXT];
    char bios_stem[INPUT_ROUTE_V3_TEXT];
    char boot_mode[INPUT_ROUTE_V3_TEXT];
    uint32_t disc_digest_kind;
    uint8_t disc_digest[32];
    uint32_t marker_count;
    uint32_t checkpoint_count;
} InputRouteV3;

static inline uint64_t input_route_le64(const unsigned char *p)
{
    return (uint64_t)input_route_le32(p) | ((uint64_t)input_route_le32(p + 4) << 32);
}

static inline const char *input_route_v3_text(FILE *f, uint32_t length, char *out)
{
    if (out[0]) return "duplicate identity tag";
    if (!length || length >= INPUT_ROUTE_V3_TEXT) return "identity text length";
    if (fread(out, 1, length, f) != length) return "short identity text";
    for (uint32_t i = 0; i < length; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e)
            { out[0] = 0; return "identity text byte"; }
    out[length] = 0;
    return NULL;
}

/* Reads a complete PSXRTI3 file from the current position (the magic). Steps
 * go to `digital` for record_size 8 and `dualshock` for 12; a NULL array
 * refuses that record layout. `markers` and `checkpoints` hold up to
 * INPUT_ROUTE_V3_MAX_MARKERS entries each. `meta` is published only when the
 * whole file passes. */
static inline const char *input_route_v3_read(
    FILE *f, InputRouteV3 *meta, InputRouteStep *digital,
    InputDualShockRouteStep *dualshock, InputRouteMarker *markers,
    InputRouteCheckpoint *checkpoints)
{
    unsigned char h[INPUT_ROUTE_V3_HEADER_BYTES], e[8], small[36];
    unsigned char *cp = NULL;
    InputRouteV3 s;
    long start, size;
    uint32_t used = 0, identity = 0, ext;
    const char *error = NULL;
    memset(meta, 0, sizeof(*meta));
    memset(&s, 0, sizeof(s));
    if (!f || (start = ftell(f)) < 0) return "unseekable route";
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) return "short header";
    if (memcmp(h, "PSXRTI3\0", 8) || input_route_le32(h + 8) != 3)
        return "header identity";
    s.record_size = input_route_le32(h + 12);
    s.frames = input_route_le32(h + 16);
    ext = input_route_le32(h + 24);
    if (s.record_size != 8 && s.record_size != INPUT_DUALSHOCK_ROUTE_RECORD_BYTES)
        return "record size";
    if (!s.frames || s.frames > INPUT_ROUTE_MAX_FRAMES) return "frame count";
    if (input_route_le32(h + 20)) return "flags";
    if (ext % 4 || ext > INPUT_ROUTE_V3_MAX_EXT) return "extension size";
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 ||
        fseek(f, start + (long)sizeof(h), SEEK_SET)) return "unseekable route";
    if ((uint64_t)(size - start) != (uint64_t)sizeof(h) + ext +
                                   (uint64_t)s.frames * s.record_size)
        return "file size";
    while (!error && used < ext) {
        uint32_t tag, length, padded;
        if (ext - used < sizeof(e) || fread(e, 1, sizeof(e), f) != sizeof(e))
            { error = "short extension entry"; break; }
        used += sizeof(e);
        tag = input_route_le32(e);
        length = input_route_le32(e + 4);
        padded = (uint32_t)(((uint64_t)length + 3u) & ~(uint64_t)3u);
        if (length > ext - used || padded > ext - used)
            { error = "extension length"; break; }
        switch (tag) {
        case INPUT_ROUTE_TAG_PIN:
            error = input_route_v3_text(f, length, s.pin);
            if (!error && length != 40) error = "pin length";
            for (uint32_t i = 0; !error && i < 40; ++i)
                if (!((s.pin[i] >= '0' && s.pin[i] <= '9') ||
                      (s.pin[i] >= 'a' && s.pin[i] <= 'f'))) error = "pin hex";
            identity |= 1u;
            break;
        case INPUT_ROUTE_TAG_DISC_SERIAL:
            error = input_route_v3_text(f, length, s.disc_serial); identity |= 2u; break;
        case INPUT_ROUTE_TAG_BIOS_STEM:
            error = input_route_v3_text(f, length, s.bios_stem); identity |= 4u; break;
        case INPUT_ROUTE_TAG_BOOT_MODE:
            error = input_route_v3_text(f, length, s.boot_mode); identity |= 8u;
            if (!error && strcmp(s.boot_mode, "hle") && strcmp(s.boot_mode, "lle") &&
                strcmp(s.boot_mode, "hle-calls") && strcmp(s.boot_mode, "hle-boot"))
                error = "boot mode";
            break;
        case INPUT_ROUTE_TAG_DISC_DIGEST:
            if (s.disc_digest_kind) { error = "duplicate identity tag"; break; }
            if (length != 36 || fread(small, 1, 36, f) != 36) { error = "disc digest length"; break; }
            s.disc_digest_kind = small[0];
            if ((small[0] != INPUT_ROUTE_DISC_DIGEST_CUE &&
                 small[0] != INPUT_ROUTE_DISC_DIGEST_FILE) || small[1] || small[2] || small[3])
                error = "disc digest kind";
            memcpy(s.disc_digest, small + 4, 32);
            identity |= 16u;
            break;
        case INPUT_ROUTE_TAG_MARKER: {
            InputRouteMarker m;
            if (!markers) { error = "markers not admitted"; break; }
            if (length != INPUT_ROUTE_MARKER_BYTES ||
                fread(small, 1, INPUT_ROUTE_MARKER_BYTES, f) != INPUT_ROUTE_MARKER_BYTES)
                { error = "marker length"; break; }
            m.frame = input_route_le32(small);
            m.kind = small[4];
            if ((m.kind != INPUT_ROUTE_MARKER_MENU && m.kind != INPUT_ROUTE_MARKER_GAMEPLAY) ||
                small[5] || small[6] || small[7]) error = "marker kind";
            else if (m.frame > s.frames) error = "marker frame";
            else if (s.marker_count && m.frame <= markers[s.marker_count - 1].frame)
                error = "marker order";
            else if (s.marker_count == INPUT_ROUTE_V3_MAX_MARKERS) error = "marker capacity";
            else markers[s.marker_count++] = m;
            break;
        }
        case INPUT_ROUTE_TAG_CHECKPOINT: {
            InputRouteCheckpoint *c;
            if (!checkpoints) { error = "checkpoints not admitted"; break; }
            if (s.checkpoint_count == INPUT_ROUTE_V3_MAX_MARKERS) { error = "checkpoint capacity"; break; }
            if (length != INPUT_ROUTE_CHECKPOINT_BYTES) { error = "checkpoint length"; break; }
            if (!cp && !(cp = (unsigned char *)malloc(INPUT_ROUTE_CHECKPOINT_BYTES)))
                { error = "checkpoint memory"; break; }
            if (fread(cp, 1, length, f) != length) { error = "short checkpoint"; break; }
            c = &checkpoints[s.checkpoint_count];
            c->frame = input_route_le32(cp);
            c->cycle = input_route_le64(cp + 8);
            memcpy(c->ram_sha256, cp + 16, 32);
            for (uint32_t i = 0; i < INPUT_ROUTE_RAM_PAGES; ++i)
                c->pages[i] = input_route_le64(cp + 48 + 8 * i);
            if (input_route_le32(cp + 4)) error = "checkpoint reserved";
            else if (c->frame > s.frames) error = "checkpoint frame";
            else if (s.checkpoint_count && c->frame <= checkpoints[s.checkpoint_count - 1].frame)
                error = "checkpoint order";
            else ++s.checkpoint_count;
            break;
        }
        default:
            if (!(tag & INPUT_ROUTE_TAG_SKIPPABLE)) { error = "unsupported mandatory extension tag"; break; }
            if ((tag & 0x7fffff00u) == 0x100u) { error = "unsupported identity/marker tag"; break; }
            if (fseek(f, (long)length, SEEK_CUR)) error = "short extension payload";
            break;
        }
        for (uint32_t i = length; !error && i < padded; ++i)
            if (fgetc(f) != 0) error = "extension padding";
        used += padded;
    }
    free(cp);
    if (error) return error;
    if (identity && identity != 31u) return "partial identity";
    s.has_identity = identity == 31u;
    for (uint32_t i = 0, j = 0; i < s.checkpoint_count; ++i) {
        while (j < s.marker_count && markers[j].frame < checkpoints[i].frame) ++j;
        if (j == s.marker_count || markers[j].frame != checkpoints[i].frame)
            return "checkpoint without marker";
    }
    if (s.record_size == 8) {
        if (!digital) return "digital records not admitted";
        error = input_route_read_records(f, s.frames, digital, &s.steps);
    } else {
        if (!dualshock) return "DualShock records not admitted";
        error = input_dualshock_route_read_records(f, s.frames, dualshock, &s.steps);
    }
    if (error) return error;
    if (fgetc(f) != EOF || ferror(f)) return "trailing bytes/read error";
    *meta = s;
    return NULL;
}

static inline void input_route_put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static inline int input_route_v3_put_entry(FILE *f, uint32_t tag,
                                           const unsigned char *payload,
                                           uint32_t length)
{
    static const unsigned char zero[4] = {0, 0, 0, 0};
    unsigned char e[8];
    input_route_put32(e, tag);
    input_route_put32(e + 4, length);
    return fwrite(e, 1, 8, f) == 8 &&
           fwrite(payload, 1, length, f) == length &&
           fwrite(zero, 1, (4u - length % 4u) % 4u, f) == (4u - length % 4u) % 4u;
}

static inline uint32_t input_route_v3_entry_bytes(uint32_t length)
{
    return 8u + ((length + 3u) & ~3u);
}

/* Writes a digital PSXRTI3 route: identity when meta->has_identity, then
 * `meta->marker_count` markers and `meta->checkpoint_count` checkpoints, then
 * one record per word. The caller validates ordering; the reader re-checks. */
static inline const char *input_route_v3_write_digital(
    FILE *f, const InputRouteV3 *meta, const uint16_t *words, uint32_t frames,
    const InputRouteMarker *markers, const InputRouteCheckpoint *checkpoints)
{
    unsigned char h[INPUT_ROUTE_V3_HEADER_BYTES], small[36], r[8];
    unsigned char cp[INPUT_ROUTE_CHECKPOINT_BYTES];
    uint64_t ext = 0;
    const char *text[4] = {meta->pin, meta->disc_serial, meta->bios_stem, meta->boot_mode};
    static const uint32_t text_tag[4] = {INPUT_ROUTE_TAG_PIN, INPUT_ROUTE_TAG_DISC_SERIAL,
                                         INPUT_ROUTE_TAG_BIOS_STEM, INPUT_ROUTE_TAG_BOOT_MODE};
    if (!frames || frames > INPUT_ROUTE_MAX_FRAMES) return "frame count";
    if (meta->marker_count > INPUT_ROUTE_V3_MAX_MARKERS ||
        meta->checkpoint_count > INPUT_ROUTE_V3_MAX_MARKERS) return "marker capacity";
    if (meta->has_identity) {
        for (unsigned i = 0; i < 4; ++i)
            ext += input_route_v3_entry_bytes((uint32_t)strlen(text[i]));
        ext += input_route_v3_entry_bytes(36);
    }
    ext += (uint64_t)meta->marker_count * input_route_v3_entry_bytes(INPUT_ROUTE_MARKER_BYTES);
    ext += (uint64_t)meta->checkpoint_count * input_route_v3_entry_bytes(INPUT_ROUTE_CHECKPOINT_BYTES);
    if (ext > INPUT_ROUTE_V3_MAX_EXT) return "extension size";
    memcpy(h, "PSXRTI3\0", 8);
    input_route_put32(h + 8, 3);
    input_route_put32(h + 12, 8);
    input_route_put32(h + 16, frames);
    input_route_put32(h + 20, 0);
    input_route_put32(h + 24, (uint32_t)ext);
    if (fwrite(h, 1, sizeof(h), f) != sizeof(h)) return "write header";
    if (meta->has_identity) {
        for (unsigned i = 0; i < 4; ++i)
            if (!input_route_v3_put_entry(f, text_tag[i], (const unsigned char *)text[i],
                                          (uint32_t)strlen(text[i])))
                return "write identity";
        memset(small, 0, sizeof(small));
        small[0] = (unsigned char)meta->disc_digest_kind;
        memcpy(small + 4, meta->disc_digest, 32);
        if (!input_route_v3_put_entry(f, INPUT_ROUTE_TAG_DISC_DIGEST, small, 36))
            return "write identity";
    }
    for (uint32_t i = 0; i < meta->marker_count; ++i) {
        memset(small, 0, INPUT_ROUTE_MARKER_BYTES);
        input_route_put32(small, markers[i].frame);
        small[4] = (unsigned char)markers[i].kind;
        if (!input_route_v3_put_entry(f, INPUT_ROUTE_TAG_MARKER, small, INPUT_ROUTE_MARKER_BYTES))
            return "write marker";
    }
    for (uint32_t i = 0; i < meta->checkpoint_count; ++i) {
        const InputRouteCheckpoint *c = &checkpoints[i];
        memset(cp, 0, sizeof(cp));
        input_route_put32(cp, c->frame);
        input_route_put32(cp + 8, (uint32_t)c->cycle);
        input_route_put32(cp + 12, (uint32_t)(c->cycle >> 32));
        memcpy(cp + 16, c->ram_sha256, 32);
        for (uint32_t p = 0; p < INPUT_ROUTE_RAM_PAGES; ++p) {
            input_route_put32(cp + 48 + 8 * p, (uint32_t)c->pages[p]);
            input_route_put32(cp + 52 + 8 * p, (uint32_t)(c->pages[p] >> 32));
        }
        if (!input_route_v3_put_entry(f, INPUT_ROUTE_TAG_CHECKPOINT, cp, sizeof(cp)))
            return "write checkpoint";
    }
    for (uint32_t i = 0; i < frames; ++i) {
        input_route_put32(r, i + 1);
        r[4] = (unsigned char)words[i];
        r[5] = (unsigned char)(words[i] >> 8);
        r[6] = r[7] = 0;
        if (fwrite(r, 1, sizeof(r), f) != sizeof(r)) return "write record";
    }
    return ferror(f) ? "write error" : NULL;
}
#endif
