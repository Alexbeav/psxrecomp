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
#include "input_replay_devices.h"
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
/* PS1B-191 player replay. Mandatory tags: a reader that does not admit
 * replays refuses the file instead of replaying it from power-on.
 *   ANCHOR    the machine state (.pst blob) the replay starts from
 *   SETTINGS  ASCII "key=value" lines of the host settings that change
 *             guest timing, switched on for playback and restored after
 *   DIGESTS   u32 count, then count entries of u32 frame and the u32 core,
 *             av, aux and ext rollback state digests at that boundary
 *             (netplay_state_digest.h), frames strictly increasing */
#define INPUT_ROUTE_TAG_REPLAY_ANCHOR   0x00000301u
#define INPUT_ROUTE_TAG_REPLAY_SETTINGS 0x00000302u
#define INPUT_ROUTE_TAG_REPLAY_DIGESTS  0x00000303u
#define INPUT_ROUTE_REPLAY_DIGEST_BYTES 20u
/* Optional presentation entries (skippable):
 *   THUMB  u16 width, u16 height, then width*height u32 ARGB (little endian),
 *          the display at the anchor
 *   NAME   UTF-8 text, no control bytes, at most INPUT_ROUTE_REPLAY_NAME_MAX */
#define INPUT_ROUTE_TAG_REPLAY_THUMB    0x80000304u
#define INPUT_ROUTE_TAG_REPLAY_NAME     0x80000305u
#define INPUT_ROUTE_REPLAY_NAME_MAX     96u
#define INPUT_ROUTE_REPLAY_THUMB_MAX    (4u + 4u * 256u * 256u)
#define INPUT_ROUTE_REPLAY_SETTINGS_MAX 4096u
/* PS1B-316 power-on replay: no ANCHOR, the replay starts at a cold boot.
 * Mandatory tags; a replay holds exactly one of ANCHOR and POWER_ON.
 *   POWER_ON  u32 vblank (always 0): record 0 is delivered at the first
 *             vblank after power-on, so playback must start the same way
 *   CARDS     u32 mask (bit 0 card 1, bit 1 card 2 inserted), then one
 *             INPUT_ROUTE_REPLAY_CARD_BYTES image per inserted card, in slot
 *             order: the cards as the guest found them at power-on
 * Optional (skippable):
 *   PRODUCT   ASCII "key=value" lines naming the product that recorded it:
 *             exe_sha256, codegen, bios_crc32, renderer, input_seed */
#define INPUT_ROUTE_TAG_REPLAY_POWER_ON 0x00000306u
#define INPUT_ROUTE_TAG_REPLAY_CARDS    0x00000307u
#define INPUT_ROUTE_TAG_REPLAY_PRODUCT  0x80000308u
#define INPUT_ROUTE_REPLAY_CARD_BYTES   (128u * 1024u)
#define INPUT_ROUTE_REPLAY_PRODUCT_MAX  1024u
/* PS1B-344: PSXRTI4 only; final inputs for both standalone console ports. */
#define INPUT_ROUTE_TAG_REPLAY_DEVICES  0x00000309u

/* Disc digest kinds. CUE: SHA-256 over the ASCII hex digests of the cue file
 * and then each FILE track in cue order, joined by '\n', no trailing newline
 * (run_native.checkpoint_asset_digest). FILE: SHA-256 of the whole image. */
#define INPUT_ROUTE_DISC_DIGEST_CUE  1u
#define INPUT_ROUTE_DISC_DIGEST_FILE 2u

#define INPUT_ROUTE_MARKER_MENU     1u
#define INPUT_ROUTE_MARKER_GAMEPLAY 2u
#define INPUT_ROUTE_MARKER_END      3u /* replay only: the last boundary */
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

/* Replay extension read by input_route_v3_read_ex. The anchor is located, not
 * loaded: anchor_offset is its byte offset from the start of the file. */
typedef struct {
    int has_anchor;
    long anchor_offset;
    uint32_t anchor_length;
    uint32_t settings_length;
    char settings[INPUT_ROUTE_REPLAY_SETTINGS_MAX + 1];
    uint32_t digest_count;   /* DIGESTS entries, located like the anchor */
    uint32_t digest_length;
    long digest_offset;
    uint32_t thumb_w, thumb_h; /* 0 when absent; pixels at thumb_offset */
    long thumb_offset;
    char name[INPUT_ROUTE_REPLAY_NAME_MAX + 1];
    int power_on;              /* POWER_ON present (then has_anchor is 0) */
    int has_cards;
    uint32_t cards_mask;       /* card images located at cards_offset */
    long cards_offset;
    char product[INPUT_ROUTE_REPLAY_PRODUCT_MAX + 1];
    long devices_offset;
    uint32_t devices_length, devices_count;
    uint8_t device_profile[2];
    InputReplayDevicePort device_initial[2];
} InputRouteV3Replay;

/* Replay entries for input_route_v3_write_ex; NULL members are left out. */
typedef struct {
    const void *anchor;
    uint32_t anchor_length;
    const char *settings;
    const void *digests;
    uint32_t digests_length;
    const uint32_t *thumb;   /* thumb_w * thumb_h ARGB */
    uint16_t thumb_w, thumb_h;
    const char *name;
    int power_on;            /* write POWER_ON (with no anchor) */
    const unsigned char *cards;   /* 128 KiB per bit set in cards_mask */
    uint32_t cards_mask;          /* written when power_on */
    const char *product;
    const InputReplayDeviceRun *devices;
    uint32_t devices_count;
    const InputReplayDevicePort *device_initial;
} InputRouteV3ReplayOut;

static inline uint32_t input_route_replay_card_count(uint32_t mask)
{
    return (mask & 1u) + ((mask >> 1) & 1u);
}

static inline int input_route_v3_name_ok(const char *s, size_t n)
{
    if (!n || n > INPUT_ROUTE_REPLAY_NAME_MAX) return 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f) return 0;
    return 1;
}

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

/* Reads PSXRTI3, or PSXRTI4 only with replay admission. Steps
 * go to `digital` for record_size 8 and `dualshock` for 12; a NULL array
 * refuses that record layout. `markers` and `checkpoints` hold up to
 * INPUT_ROUTE_V3_MAX_MARKERS entries each. `meta` is published only when the
 * whole file passes. */
/* `replay` NULL refuses the replay tags and the END marker, as before PS1B-191;
 * non-NULL admits them and receives the anchor location and settings. */
static inline const char *input_route_v3_read_ex(
    FILE *f, InputRouteV3 *meta, InputRouteStep *digital,
    InputDualShockRouteStep *dualshock, InputRouteMarker *markers,
    InputRouteCheckpoint *checkpoints, InputRouteV3Replay *replay)
{
    InputRouteV3Replay rp;
    memset(&rp, 0, sizeof(rp));
    if (replay) memset(replay, 0, sizeof(*replay));
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
    const int v4 = !memcmp(h, "PSXRTI4\0", 8) && input_route_le32(h + 8) == 4;
    if (!v4 && (memcmp(h, "PSXRTI3\0", 8) || input_route_le32(h + 8) != 3))
        return "header identity";
    if (v4 && !replay) return "device replay not admitted";
    s.record_size = input_route_le32(h + 12);
    s.frames = input_route_le32(h + 16);
    ext = input_route_le32(h + 24);
    if (s.record_size != 8 && s.record_size != INPUT_DUALSHOCK_ROUTE_RECORD_BYTES)
        return "record size";
    if (v4 && s.record_size != INPUT_DUALSHOCK_ROUTE_RECORD_BYTES) return "device replay record size";
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
            if ((m.kind != INPUT_ROUTE_MARKER_MENU && m.kind != INPUT_ROUTE_MARKER_GAMEPLAY &&
                 !(replay && m.kind == INPUT_ROUTE_MARKER_END)) ||
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
        case INPUT_ROUTE_TAG_REPLAY_ANCHOR:
            if (!replay) { error = "unsupported mandatory extension tag"; break; }
            if (rp.has_anchor) { error = "duplicate replay anchor"; break; }
            if (!length) { error = "replay anchor length"; break; }
            rp.has_anchor = 1;
            rp.anchor_offset = ftell(f) - start;
            rp.anchor_length = length;
            if (fseek(f, (long)length, SEEK_CUR)) error = "short replay anchor";
            break;
        case INPUT_ROUTE_TAG_REPLAY_SETTINGS:
            if (!replay) { error = "unsupported mandatory extension tag"; break; }
            if (rp.settings_length) { error = "duplicate replay settings"; break; }
            if (!length || length > INPUT_ROUTE_REPLAY_SETTINGS_MAX) { error = "replay settings length"; break; }
            if (fread(rp.settings, 1, length, f) != length) { error = "short replay settings"; break; }
            for (uint32_t i = 0; !error && i < length; ++i)
                if ((unsigned char)rp.settings[i] != '\n' &&
                    ((unsigned char)rp.settings[i] < 0x20 || (unsigned char)rp.settings[i] > 0x7e))
                    error = "replay settings byte";
            rp.settings[length] = 0;
            rp.settings_length = length;
            break;
        case INPUT_ROUTE_TAG_REPLAY_DIGESTS:
            if (!replay) { error = "unsupported mandatory extension tag"; break; }
            if (rp.digest_length) { error = "duplicate replay digests"; break; }
            if (length < 4 || fread(small, 1, 4, f) != 4) { error = "replay digests length"; break; }
            rp.digest_count = input_route_le32(small);
            if (rp.digest_count > s.frames + 1u ||
                length != 4u + rp.digest_count * INPUT_ROUTE_REPLAY_DIGEST_BYTES)
                { error = "replay digests length"; break; }
            rp.digest_length = length;
            rp.digest_offset = ftell(f) - start - 4;   /* the payload, count first */
            if (fseek(f, (long)(length - 4u), SEEK_CUR)) error = "short replay digests";
            break;
        case INPUT_ROUTE_TAG_REPLAY_THUMB:
            if (!replay) { if (fseek(f, (long)length, SEEK_CUR)) error = "short extension payload"; break; }
            if (rp.thumb_w) { error = "duplicate replay thumbnail"; break; }
            if (length < 4 || length > INPUT_ROUTE_REPLAY_THUMB_MAX || fread(small, 1, 4, f) != 4)
                { error = "replay thumbnail length"; break; }
            rp.thumb_w = (uint32_t)small[0] | ((uint32_t)small[1] << 8);
            rp.thumb_h = (uint32_t)small[2] | ((uint32_t)small[3] << 8);
            if (!rp.thumb_w || !rp.thumb_h || length != 4u + 4u * rp.thumb_w * rp.thumb_h)
                { rp.thumb_w = rp.thumb_h = 0; error = "replay thumbnail length"; break; }
            rp.thumb_offset = ftell(f) - start;
            if (fseek(f, (long)(length - 4u), SEEK_CUR)) error = "short replay thumbnail";
            break;
        case INPUT_ROUTE_TAG_REPLAY_NAME:
            if (!replay) { if (fseek(f, (long)length, SEEK_CUR)) error = "short extension payload"; break; }
            if (rp.name[0]) { error = "duplicate replay name"; break; }
            if (!length || length > INPUT_ROUTE_REPLAY_NAME_MAX ||
                fread(rp.name, 1, length, f) != length) { rp.name[0] = 0; error = "replay name length"; break; }
            rp.name[length] = 0;
            if (!input_route_v3_name_ok(rp.name, length)) { rp.name[0] = 0; error = "replay name byte"; }
            break;
        case INPUT_ROUTE_TAG_REPLAY_POWER_ON:
            if (!replay) { error = "unsupported mandatory extension tag"; break; }
            if (rp.power_on) { error = "duplicate replay power-on"; break; }
            if (length != 4 || fread(small, 1, 4, f) != 4) { error = "replay power-on length"; break; }
            if (input_route_le32(small)) { error = "replay power-on vblank"; break; }
            rp.power_on = 1;
            break;
        case INPUT_ROUTE_TAG_REPLAY_CARDS:
            if (!replay) { error = "unsupported mandatory extension tag"; break; }
            if (rp.has_cards) { error = "duplicate replay cards"; break; }
            if (length < 4 || fread(small, 1, 4, f) != 4) { error = "replay cards length"; break; }
            rp.cards_mask = input_route_le32(small);
            if (rp.cards_mask > 3u ||
                length != 4u + input_route_replay_card_count(rp.cards_mask) * INPUT_ROUTE_REPLAY_CARD_BYTES)
                { error = "replay cards length"; break; }
            rp.has_cards = 1;
            rp.cards_offset = ftell(f) - start;
            if (fseek(f, (long)(length - 4u), SEEK_CUR)) error = "short replay cards";
            break;
        case INPUT_ROUTE_TAG_REPLAY_DEVICES:
            if (!v4 || !replay) { error = "device replay not admitted"; break; }
            if (rp.devices_count) { error = "duplicate device stream"; break; }
            rp.devices_offset = ftell(f) - start;
            rp.devices_length = length;
            error = input_replay_devices_read(f, length, s.frames, NULL,
                                               rp.device_profile, rp.device_initial, &rp.devices_count);
            break;
        case INPUT_ROUTE_TAG_REPLAY_PRODUCT:
            if (!replay) { if (fseek(f, (long)length, SEEK_CUR)) error = "short extension payload"; break; }
            if (rp.product[0]) { error = "duplicate replay product"; break; }
            if (!length || length > INPUT_ROUTE_REPLAY_PRODUCT_MAX ||
                fread(rp.product, 1, length, f) != length) { rp.product[0] = 0; error = "replay product length"; break; }
            rp.product[length] = 0;
            for (uint32_t i = 0; !error && i < length; ++i)
                if ((unsigned char)rp.product[i] != '\n' &&
                    ((unsigned char)rp.product[i] < 0x20 || (unsigned char)rp.product[i] > 0x7e))
                    error = "replay product byte";
            if (error) rp.product[0] = 0;
            break;
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
    if (v4 && !rp.devices_count) return "missing device stream";
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
    if (replay) {
        /* A replay needs its anchor or a power-on start (not both), its
         * identity and an END checkpoint on the last boundary. A power-on
         * replay also carries the memory cards it booted with. */
        if (rp.has_anchor && rp.power_on) return "replay has an anchor and a power-on start";
        if (!rp.has_anchor && !rp.power_on) return "replay anchor missing";
        if (rp.power_on && !rp.has_cards) return "replay cards missing";
        if (!s.has_identity) return "replay identity missing";
        if (!s.marker_count || markers[s.marker_count - 1].kind != INPUT_ROUTE_MARKER_END ||
            markers[s.marker_count - 1].frame != s.frames)
            return "replay END marker missing";
        if (!s.checkpoint_count || checkpoints[s.checkpoint_count - 1].frame != s.frames)
            return "replay END checkpoint missing";
        *replay = rp;
    }
    *meta = s;
    return NULL;
}

static inline const char *input_route_v3_read(
    FILE *f, InputRouteV3 *meta, InputRouteStep *digital,
    InputDualShockRouteStep *dualshock, InputRouteMarker *markers,
    InputRouteCheckpoint *checkpoints)
{
    return input_route_v3_read_ex(f, meta, digital, dualshock, markers, checkpoints, NULL);
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

/* One DualShock record: active-low buttons and protocol axes LY,LX,RY,RX. */
typedef struct {
    uint16_t buttons;
    uint8_t axes_ly_lx_ry_rx[4];
} InputRouteDualShockWord;

/* Writes PSXRTI3, or PSXRTI4 with mandatory rx->devices: identity when meta->has_identity, then the replay
 * anchor and settings when given, then `meta->marker_count` markers and
 * `meta->checkpoint_count` checkpoints, then one record per frame: `words`
 * (record size 8) or `dual` (record size 12), exactly one of them non-NULL.
 * The caller validates ordering; the reader re-checks. */
static inline const char *input_route_v3_write_ex(
    FILE *f, const InputRouteV3 *meta, const uint16_t *words,
    const InputRouteDualShockWord *dual, uint32_t frames,
    const InputRouteMarker *markers, const InputRouteCheckpoint *checkpoints,
    const InputRouteV3ReplayOut *rx)
{
    const void *anchor = rx ? rx->anchor : NULL;
    const uint32_t anchor_length = rx ? rx->anchor_length : 0;
    const char *settings = rx ? rx->settings : NULL;
    const void *digests = rx ? rx->digests : NULL;
    const uint32_t digests_length = rx ? rx->digests_length : 0;
    const uint32_t thumb_bytes = rx && rx->thumb ? 4u + 4u * rx->thumb_w * rx->thumb_h : 0;
    const size_t name_len = rx && rx->name ? strlen(rx->name) : 0;
    const int power_on = rx && rx->power_on;
    const uint32_t cards_mask = power_on ? rx->cards_mask : 0;
    const uint32_t cards_length =
        4u + input_route_replay_card_count(cards_mask) * INPUT_ROUTE_REPLAY_CARD_BYTES;
    const size_t product_len = rx && rx->product ? strlen(rx->product) : 0;
    const uint32_t device_count = rx && rx->devices
        ? input_replay_devices_prefix(rx->devices, rx->devices_count, frames) : 0;
    const uint32_t device_bytes = device_count
        ? INPUT_REPLAY_DEVICE_HEADER_BYTES + device_count * INPUT_REPLAY_DEVICE_RUN_BYTES : 0;
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
    if (!words == !dual) return "record layout";
    if (device_count && words) return "device replay record size";
    if (rx && rx->devices && (!device_count || (!anchor && !power_on)))
        return "device replay frame coverage/start";
    if (device_bytes) ext += input_route_v3_entry_bytes(device_bytes);
    if (anchor && power_on) return "replay anchor and power-on";
    if (anchor) ext += input_route_v3_entry_bytes(anchor_length);
    if (power_on) {
        if (cards_mask > 3u || (cards_mask && !rx->cards)) return "replay cards";
        ext += input_route_v3_entry_bytes(4) + input_route_v3_entry_bytes(cards_length);
    }
    if (product_len) {
        if (product_len > INPUT_ROUTE_REPLAY_PRODUCT_MAX) return "replay product length";
        for (size_t i = 0; i < product_len; ++i)
            if ((unsigned char)rx->product[i] != '\n' &&
                ((unsigned char)rx->product[i] < 0x20 || (unsigned char)rx->product[i] > 0x7e))
                return "replay product byte";
        ext += input_route_v3_entry_bytes((uint32_t)product_len);
    }
    if (settings && settings[0]) {
        if (strlen(settings) > INPUT_ROUTE_REPLAY_SETTINGS_MAX) return "replay settings length";
        ext += input_route_v3_entry_bytes((uint32_t)strlen(settings));
    }
    if (digests) ext += input_route_v3_entry_bytes(digests_length);
    if (thumb_bytes) {
        if (!rx->thumb_w || !rx->thumb_h || thumb_bytes > INPUT_ROUTE_REPLAY_THUMB_MAX) return "replay thumbnail size";
        ext += input_route_v3_entry_bytes(thumb_bytes);
    }
    if (name_len) {
        if (!input_route_v3_name_ok(rx->name, name_len)) return "replay name";
        ext += input_route_v3_entry_bytes((uint32_t)name_len);
    }
    ext += (uint64_t)meta->marker_count * input_route_v3_entry_bytes(INPUT_ROUTE_MARKER_BYTES);
    ext += (uint64_t)meta->checkpoint_count * input_route_v3_entry_bytes(INPUT_ROUTE_CHECKPOINT_BYTES);
    if (ext > INPUT_ROUTE_V3_MAX_EXT) return "extension size";
    memcpy(h, device_count ? "PSXRTI4\0" : "PSXRTI3\0", 8);
    input_route_put32(h + 8, device_count ? 4 : 3);
    input_route_put32(h + 12, words ? 8u : INPUT_DUALSHOCK_ROUTE_RECORD_BYTES);
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
    if (anchor && !input_route_v3_put_entry(f, INPUT_ROUTE_TAG_REPLAY_ANCHOR,
                                            (const unsigned char *)anchor, anchor_length))
        return "write replay anchor";
    if (power_on) {
        unsigned char e8[8];
        const uint32_t image_bytes = cards_length - 4u;
        memset(small, 0, 4);
        if (!input_route_v3_put_entry(f, INPUT_ROUTE_TAG_REPLAY_POWER_ON, small, 4))
            return "write replay power-on";
        input_route_put32(e8, INPUT_ROUTE_TAG_REPLAY_CARDS);
        input_route_put32(e8 + 4, cards_length);
        input_route_put32(small, cards_mask);
        if (fwrite(e8, 1, 8, f) != 8 || fwrite(small, 1, 4, f) != 4 ||
            (image_bytes && fwrite(rx->cards, 1, image_bytes, f) != image_bytes))
            return "write replay cards";
    }
    if (product_len && !input_route_v3_put_entry(f, INPUT_ROUTE_TAG_REPLAY_PRODUCT,
                                                 (const unsigned char *)rx->product,
                                                 (uint32_t)product_len))
        return "write replay product";
    if (device_bytes) {
        unsigned char e8[8];
        input_route_put32(e8, INPUT_ROUTE_TAG_REPLAY_DEVICES);
        input_route_put32(e8 + 4, device_bytes);
        if (fwrite(e8, 1, 8, f) != 8) return "write device entry";
        const char *device_error = input_replay_devices_write(f, rx->devices,
                                                              rx->devices_count, frames, rx->device_initial);
        if (device_error) return device_error;
    }
    if (settings && settings[0] &&
        !input_route_v3_put_entry(f, INPUT_ROUTE_TAG_REPLAY_SETTINGS,
                                  (const unsigned char *)settings, (uint32_t)strlen(settings)))
        return "write replay settings";
    if (digests && !input_route_v3_put_entry(f, INPUT_ROUTE_TAG_REPLAY_DIGESTS,
                                             (const unsigned char *)digests, digests_length))
        return "write replay digests";
    if (thumb_bytes) {
        unsigned char e8[8];
        input_route_put32(e8, INPUT_ROUTE_TAG_REPLAY_THUMB);
        input_route_put32(e8 + 4, thumb_bytes);
        small[0] = (unsigned char)rx->thumb_w; small[1] = (unsigned char)(rx->thumb_w >> 8);
        small[2] = (unsigned char)rx->thumb_h; small[3] = (unsigned char)(rx->thumb_h >> 8);
        if (fwrite(e8, 1, 8, f) != 8 || fwrite(small, 1, 4, f) != 4) return "write replay thumbnail";
        for (uint32_t i = 0; i < (uint32_t)rx->thumb_w * rx->thumb_h; ++i) {
            unsigned char px[4];
            input_route_put32(px, rx->thumb[i]);
            if (fwrite(px, 1, 4, f) != 4) return "write replay thumbnail";
        }
    }
    if (name_len && !input_route_v3_put_entry(f, INPUT_ROUTE_TAG_REPLAY_NAME,
                                              (const unsigned char *)rx->name, (uint32_t)name_len))
        return "write replay name";
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
        unsigned char d[INPUT_DUALSHOCK_ROUTE_RECORD_BYTES];
        if (words) {
            input_route_put32(r, i + 1);
            r[4] = (unsigned char)words[i];
            r[5] = (unsigned char)(words[i] >> 8);
            r[6] = r[7] = 0;
            if (fwrite(r, 1, sizeof(r), f) != sizeof(r)) return "write record";
        } else {
            input_route_put32(d, i + 1);
            d[4] = (unsigned char)dual[i].buttons;
            d[5] = (unsigned char)(dual[i].buttons >> 8);
            memcpy(d + 6, dual[i].axes_ly_lx_ry_rx, 4);
            d[10] = d[11] = 0;
            if (fwrite(d, 1, sizeof(d), f) != sizeof(d)) return "write record";
        }
    }
    return ferror(f) ? "write error" : NULL;
}

static inline const char *input_route_v3_write_digital(
    FILE *f, const InputRouteV3 *meta, const uint16_t *words, uint32_t frames,
    const InputRouteMarker *markers, const InputRouteCheckpoint *checkpoints)
{
    return input_route_v3_write_ex(f, meta, words, NULL, frames, markers, checkpoints, NULL);
}
#endif
