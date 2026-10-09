/* Player replay recorder and player (PS1B-191). See replay_session.h. */
#include "replay_session.h"
#include "psx_sha256.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define RAM_BYTES (2u << 20)
#define PAGE_BYTES 4096u
#define PATH_BYTES 1024u
/* A stick counts as input past this distance from centre (0x80). */
#define TAKEOVER_DEADZONE 0x30

static ReplayState s_state = REPLAY_IDLE;
static ReplayResult s_result = REPLAY_RESULT_NONE;
static unsigned s_play_core_digest_version = 3u;

unsigned replay_session_core_digest_version(void)
{
    return s_state == REPLAY_PLAYING || s_state == REPLAY_LOADING
        ? s_play_core_digest_version : 3u;
}

/* Recording. */
static char s_rec_path[PATH_BYTES];
static int s_rec_slot = -1;
static int s_stop_requested;
static InputRouteDualShockWord *s_words;
static uint32_t s_frames, s_steps;
static InputRouteV3 s_meta;
static char s_settings[INPUT_ROUTE_REPLAY_SETTINGS_MAX + 1];
static uint8_t *s_anchor;
static size_t s_anchor_size;
/* RAM and cycle at the last recorded boundary, so a recording cut short by
 * process exit still ends on a checkpoint that playback reaches. */
static uint8_t *s_last_ram;
static uint64_t s_last_cycle;
static uint32_t s_last_frame;
/* Thumbnail at the anchor and the wall-clock start, for the default name. */
static uint32_t *s_thumb;
static int s_have_thumb;
static time_t s_rec_started;
/* Rollback state digests: frame, core, av, aux, ext. */
#define DIGEST_CAP (INPUT_ROUTE_MAX_FRAMES / REPLAY_DIGEST_INTERVAL + 2u)
static uint32_t (*s_digests)[5];
static uint32_t s_digest_count;
/* Power-on recording (PS1B-316): no anchor; the cards the guest booted with
 * (both slots, 2 x INPUT_ROUTE_REPLAY_CARD_BYTES) and the product identity. */
static int s_power_on;
static uint8_t *s_cards;
static uint32_t s_cards_mask;
static char s_product[INPUT_ROUTE_REPLAY_PRODUCT_MAX + 1];

/* Playback. */
static InputDualShockRouteStep *s_steps_play;
static uint32_t s_play_steps, s_play_frames, s_play_step, s_play_left, s_play_frame;
static InputRouteCheckpoint s_end;
static int s_takeover_armed;
static int s_settings_switched;
static uint32_t (*s_play_digests)[5];
static uint32_t s_play_digest_count, s_play_digest_next;
static unsigned s_digests_checked, s_div_parts;
static uint32_t s_div_frame;
static int s_diverged;
static int s_play_power_on, s_cards_installed;

static const char *digest_parts_text(unsigned parts, char *out, size_t cap);

/* Verdict file (replay_session_set_verdict_path) and what it reports. */
static char s_verdict_path[PATH_BYTES];
static char s_play_path[PATH_BYTES];
static char s_rec_pin[INPUT_ROUTE_V3_TEXT], s_player_pin[INPUT_ROUTE_V3_TEXT];
static char s_rec_exe[65], s_player_exe[65];
/* Product lines of the recording and of this player: "platform" (for example
 * windows-x64) and "codegen". s_cross_platform is 1 when the same pin plays on
 * another platform's build. */
static char s_rec_platform[24], s_player_platform[24];
static char s_rec_codegen[16], s_player_codegen[16];
static int s_cross_platform;
static uint64_t s_end_cycle, s_end_recorded_cycle;
static unsigned s_end_pages;
static int s_end_reached;

void replay_session_set_verdict_path(const char *path)
{
    snprintf(s_verdict_path, sizeof s_verdict_path, "%s", path ? path : "");
}

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void write_verdict(ReplayResult result, const char *reason)
{
    static const char *const names[] = {"incomplete", "in_sync", "diverged", "stopped_by_input", "failed"};
    char parts[32];
    FILE *f;
    if (!s_verdict_path[0] || !(f = fopen(s_verdict_path, "wb"))) return;
    fprintf(f, "{\n  \"schema\": \"psxrecomp-replay-verdict/1\",\n  \"result\": \"%s\",\n",
            names[result <= REPLAY_RESULT_FAILED ? result : 0]);
    fprintf(f, "  \"frames_played\": %u,\n  \"frames_total\": %u,\n",
            (unsigned)s_play_frame, (unsigned)s_play_frames);
    if (s_diverged) fprintf(f, "  \"first_divergence_frame\": %u,\n  \"divergence_parts\": \"%s\",\n",
                            (unsigned)s_div_frame, digest_parts_text(s_div_parts, parts, sizeof parts));
    else fprintf(f, "  \"first_divergence_frame\": null,\n  \"divergence_parts\": null,\n");
    fprintf(f, "  \"digests_checked\": %u,\n", s_digests_checked);
    if (s_end_reached)
        fprintf(f, "  \"end_cycle\": %llu,\n  \"recorded_end_cycle\": %llu,\n  \"differing_ram_pages\": %u,\n",
                (unsigned long long)s_end_cycle, (unsigned long long)s_end_recorded_cycle, s_end_pages);
    fputs("  \"recorded_build\": ", f); json_string(f, s_rec_pin);
    fputs(",\n  \"player_build\": ", f); json_string(f, s_player_pin);
    fputs(",\n  \"recorded_exe_sha256\": ", f); json_string(f, s_rec_exe);
    fputs(",\n  \"player_exe_sha256\": ", f); json_string(f, s_player_exe);
    fputs(",\n  \"recorded_platform\": ", f); json_string(f, s_rec_platform);
    fputs(",\n  \"player_platform\": ", f); json_string(f, s_player_platform);
    fputs(",\n  \"recorded_codegen\": ", f); json_string(f, s_rec_codegen);
    fputs(",\n  \"player_codegen\": ", f); json_string(f, s_player_codegen);
    fprintf(f, ",\n  \"cross_platform\": %s", s_cross_platform ? "true" : "false");
    fprintf(f, ",\n  \"power_on\": %s", s_play_power_on ? "true" : "false");
    fputs(",\n  \"replay\": ", f); json_string(f, s_play_path);
    fputs(",\n  \"reason\": ", f); json_string(f, reason ? reason : "");
    fputs("\n}\n", f);
    fclose(f);
}

ReplayState replay_session_state(void) { return s_state; }
ReplayResult replay_session_last_result(void) { return s_result; }
int replay_session_owns_p1(void) { return s_state == REPLAY_RECORDING || s_state == REPLAY_PLAYING; }

int replay_session_rec_visible(uint64_t now_ms)
{
    if (s_state != REPLAY_ARMING && s_state != REPLAY_RECORDING) return 0;
    return now_ms % 1000u < 500u;   /* VCR-style 1 Hz blink */
}

/* ---- Slots ---- */

int replay_session_slot_path(int slot, char *out, size_t cap)
{
    char dir[PATH_BYTES], prefix[256];
    if (slot < 0 || slot >= REPLAY_SLOTS || !replay_host_slot_base(dir, sizeof dir, prefix, sizeof prefix))
        return 0;
    return snprintf(out, cap, "%s/%s_slot%02d.psxrpl", dir, prefix, slot + 1) < (int)cap;
}

int replay_session_slot_exists(int slot)
{
    char path[PATH_BYTES];
    FILE *f;
    if (!replay_session_slot_path(slot, path, sizeof path) || !(f = fopen(path, "rb"))) return 0;
    fclose(f);
    return 1;
}

int replay_session_next_free_slot(void)
{
    for (int s = 0; s < REPLAY_SLOTS; ++s)
        if (!replay_session_slot_exists(s)) return s;
    return -1;
}

int replay_session_delete_slot(int slot)
{
    char path[PATH_BYTES];
    return replay_session_slot_path(slot, path, sizeof path) && remove(path) == 0;
}

static int create_exclusive(const char *path, FILE **out)
{
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    *out = fd < 0 ? NULL : _fdopen(fd, "wb");
    if (!*out && fd >= 0) _close(fd);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    *out = fd < 0 ? NULL : fdopen(fd, "wb");
    if (!*out && fd >= 0) close(fd);
#endif
    return *out != NULL;
}

/* A file name from a replay name: path and reserved characters become '_',
 * the middle dot '-', other non-ASCII '_'; trailing spaces and dots go. */
static void sanitize_name(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < cap; ++p) {
        char c;
        if (p[0] == 0xC2 && p[1] == 0xB7) { c = '-'; ++p; }
        else if (*p >= 0x80) { c = '_'; while ((p[1] & 0xC0) == 0x80) ++p; }
        else if (strchr("<>:\"/\\|?*", *p) || *p < 0x20) c = '_';
        else c = (char)*p;
        out[n++] = c;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) --n;
    out[n] = 0;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

int replay_session_partial_path(const char *path, char *out, size_t cap)
{
    static const char ext[] = ".psxrpl";
    size_t n = path ? strlen(path) : 0;
    if (!n) return 0;
    if (n >= sizeof ext - 1 && !strcmp(path + n - (sizeof ext - 1), ext)) n -= sizeof ext - 1;
    return snprintf(out, cap, "%.*s.partial.psxrpl", (int)n, path) < (int)cap;
}

int replay_session_boot_path(const char *dir, const char *title, int64_t utc_seconds,
                             char *out, size_t cap)
{
    char base[49], stamp[32], partial[PATH_BYTES];
    const time_t t = (time_t)utc_seconds;
    struct tm tm_utc;
    size_t n = 0;
    /* Keep [A-Za-z0-9.-]; any other run becomes one '_'; no leading or
     * trailing '_' or '.', so the name is safe in a shell and on every host. */
    for (const unsigned char *p = (const unsigned char *)(title ? title : ""); *p && n + 1 < sizeof base; ++p) {
        const int keep = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                         (*p >= '0' && *p <= '9') || (*p == '.' && n) || *p == '-';
        if (keep) base[n++] = (char)*p;
        else if (n && base[n - 1] != '_') base[n++] = '_';
    }
    while (n && (base[n - 1] == '_' || base[n - 1] == '.')) --n;
    base[n] = 0;
    if (!base[0]) snprintf(base, sizeof base, "replay");
    if (!dir || !dir[0]) return 0;
#ifdef _WIN32
    if (gmtime_s(&tm_utc, &t)) return 0;
#else
    if (!gmtime_r(&t, &tm_utc)) return 0;
#endif
    strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm_utc);
    for (int k = 1; k < 100; ++k) {
        char suffix[8] = "";
        if (k > 1) snprintf(suffix, sizeof suffix, "-%d", k);
        if (snprintf(out, cap, "%s/%s-boot-%s%s.psxrpl", dir, base, stamp, suffix) >= (int)cap)
            return 0;
        if (!file_exists(out) && replay_session_partial_path(out, partial, sizeof partial) &&
            !file_exists(partial))
            return 1;
    }
    return 0;
}

static int read_slot_replay(int slot, InputRouteV3Replay *rp, char *path, size_t path_cap);

int replay_session_export_slot(int slot, char *out_path, size_t cap)
{
    char src[PATH_BYTES], dst[PATH_BYTES], base[REPLAY_NAME_MAX + 1] = "";
    InputRouteV3Replay *rp = (InputRouteV3Replay *)malloc(sizeof *rp);
    if (rp && read_slot_replay(slot, rp, src, sizeof src) && rp->name[0])
        sanitize_name(rp->name, base, sizeof base);
    free(rp);
    const char *dir = replay_host_export_dir();
    time_t now = time(NULL);
    struct tm tm_now;
    FILE *in, *out = NULL;
    if (!replay_session_slot_path(slot, src, sizeof src) || !dir || !dir[0]) return 0;
#ifdef _WIN32
    _mkdir(dir);
    localtime_s(&tm_now, &now);
#else
    mkdir(dir, 0755);
    localtime_r(&now, &tm_now);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm_now);
    for (int n = 0; n < 100 && !out; ++n) {
        if (base[0])
            snprintf(dst, sizeof dst, n ? "%s/%s-%d.psxrpl" : "%s/%s.psxrpl", dir, base, n);
        else
            snprintf(dst, sizeof dst, n ? "%s/%s-%s-%d.psxrpl" : "%s/%s-%s.psxrpl",
                     dir, replay_host_disc_serial(), stamp, n);
        create_exclusive(dst, &out);
    }
    if (!out || !(in = fopen(src, "rb"))) { if (out) { fclose(out); remove(dst); } return 0; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out)) ok = 0;
    if (!ok) { remove(dst); return 0; }
    snprintf(out_path, cap, "%s", dst);
    return 1;
}

/* Reads a slot's replay extension (name, thumbnail location). */
static int read_slot_replay(int slot, InputRouteV3Replay *rp, char *path, size_t path_cap)
{
    InputRouteV3 meta;
    InputDualShockRouteStep *steps;
    InputRouteMarker *markers;
    InputRouteCheckpoint *cps;
    const char *error;
    FILE *f;
    if (!replay_session_slot_path(slot, path, path_cap) || !(f = fopen(path, "rb"))) return 0;
    steps = (InputDualShockRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
    markers = (InputRouteMarker *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *markers);
    cps = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
    error = !steps || !markers || !cps ? "memory"
          : input_route_v3_read_ex(f, &meta, NULL, steps, markers, cps, rp);
    fclose(f);
    free(steps); free(markers); free(cps);
    return error == NULL;
}

int replay_session_slot_info(int slot, char *name, size_t cap, uint32_t *thumb)
{
    char path[PATH_BYTES];
    InputRouteV3Replay *rp = (InputRouteV3Replay *)malloc(sizeof *rp);
    int ok = rp && read_slot_replay(slot, rp, path, sizeof path);
    if (ok && name && cap) snprintf(name, cap, "%s", rp->name);
    if (ok && thumb && rp->thumb_w == REPLAY_THUMB_W && rp->thumb_h == REPLAY_THUMB_H) {
        FILE *f = fopen(path, "rb");
        unsigned char *raw = (unsigned char *)malloc(4u * REPLAY_THUMB_W * REPLAY_THUMB_H);
        if (f && raw && !fseek(f, rp->thumb_offset, SEEK_SET) &&
            fread(raw, 4, REPLAY_THUMB_W * REPLAY_THUMB_H, f) == REPLAY_THUMB_W * REPLAY_THUMB_H)
            for (unsigned i = 0; i < REPLAY_THUMB_W * REPLAY_THUMB_H; ++i)
                thumb[i] = input_route_le32(raw + 4 * i);
        free(raw);
        if (f) fclose(f);
    }
    free(rp);
    return ok;
}

/* Copies the file with its NAME entry replaced: every other entry and the
 * records keep their bytes, the header's extension size is adjusted. */
int replay_session_rename_slot(int slot, const char *name)
{
    char path[PATH_BYTES], tmp[PATH_BYTES + 8];
    unsigned char h[INPUT_ROUTE_V3_HEADER_BYTES], e[8];
    InputRouteV3Replay *rp;
    FILE *in = NULL, *out = NULL;
    uint32_t ext, used = 0, new_ext;
    size_t len = name ? strlen(name) : 0;
    int ok;
    if (s_state != REPLAY_IDLE || !input_route_v3_name_ok(name ? name : "", len)) return 0;
    rp = (InputRouteV3Replay *)malloc(sizeof *rp);
    ok = rp && read_slot_replay(slot, rp, path, sizeof path);
    free(rp);
    if (!ok || !(in = fopen(path, "rb"))) return 0;
    snprintf(tmp, sizeof tmp, "%s.rename", path);
    remove(tmp);
    ok = fread(h, 1, sizeof h, in) == sizeof h && create_exclusive(tmp, &out);
    ext = ok ? input_route_le32(h + 24) : 0;
    new_ext = ext;
    /* First pass: the size without the old name, plus the new one. */
    while (ok && used < ext) {
        uint32_t tag, length, padded;
        ok = fread(e, 1, 8, in) == 8;
        tag = input_route_le32(e);
        length = input_route_le32(e + 4);
        padded = (length + 3u) & ~3u;
        if (ok && tag == INPUT_ROUTE_TAG_REPLAY_NAME) new_ext -= 8u + padded;
        ok = ok && !fseek(in, (long)padded, SEEK_CUR);
        used += 8u + padded;
    }
    new_ext += input_route_v3_entry_bytes((uint32_t)len);
    ok = ok && new_ext <= INPUT_ROUTE_V3_MAX_EXT && !fseek(in, (long)sizeof h, SEEK_SET);
    if (ok) {
        input_route_put32(h + 24, new_ext);
        ok = fwrite(h, 1, sizeof h, out) == sizeof h;
    }
    /* Second pass: copy entries except the old name, then the new name. */
    used = 0;
    while (ok && used < ext) {
        uint32_t tag, length, padded;
        unsigned char buf[65536];
        ok = fread(e, 1, 8, in) == 8;
        tag = input_route_le32(e);
        length = input_route_le32(e + 4);
        padded = (length + 3u) & ~3u;
        used += 8u + padded;
        if (ok && tag == INPUT_ROUTE_TAG_REPLAY_NAME) { ok = !fseek(in, (long)padded, SEEK_CUR); continue; }
        ok = ok && fwrite(e, 1, 8, out) == 8;
        for (uint32_t left = padded; ok && left; ) {
            const size_t n = left < sizeof buf ? left : sizeof buf;
            ok = fread(buf, 1, n, in) == n && fwrite(buf, 1, n, out) == n;
            left -= (uint32_t)n;
        }
    }
    ok = ok && input_route_v3_put_entry(out, INPUT_ROUTE_TAG_REPLAY_NAME, (const unsigned char *)name, (uint32_t)len);
    while (ok) {   /* the records */
        unsigned char buf[65536];
        const size_t n = fread(buf, 1, sizeof buf, in);
        if (!n) { ok = !ferror(in); break; }
        ok = fwrite(buf, 1, n, out) == n;
    }
    fclose(in);
    if (out && fclose(out)) ok = 0;
    if (ok) {
        /* The rewritten file must read back with the new name before it
         * replaces the original. */
        InputRouteV3 meta;
        InputRouteV3Replay *check = (InputRouteV3Replay *)malloc(sizeof *check);
        InputDualShockRouteStep *steps = (InputDualShockRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
        InputRouteMarker *markers = (InputRouteMarker *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *markers);
        InputRouteCheckpoint *cps = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
        FILE *r = fopen(tmp, "rb");
        ok = check && steps && markers && cps && r &&
             !input_route_v3_read_ex(r, &meta, NULL, steps, markers, cps, check) &&
             !strcmp(check->name, name);
        if (r) fclose(r);
        free(check); free(steps); free(markers); free(cps);
    }
    if (ok) ok = remove(path) == 0 && rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}

/* ---- Checkpoints (the same hashes as input_route_session's markers) ---- */

static void checkpoint_of(uint32_t frame, const uint8_t *ram, uint64_t cycle, InputRouteCheckpoint *c)
{
    c->frame = frame;
    c->cycle = cycle;
    psx_sha256_compute(ram, RAM_BYTES, c->ram_sha256);
    for (uint32_t p = 0; p < INPUT_ROUTE_RAM_PAGES; ++p) {
        uint64_t hash = UINT64_C(14695981039346656037);
        const uint8_t *bytes = ram + p * PAGE_BYTES;
        for (uint32_t i = 0; i < PAGE_BYTES; ++i)
            hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
        c->pages[p] = hash;
    }
}

static const char *digest_parts_text(unsigned parts, char *out, size_t cap)
{
    static const char *const names[4] = {"core", "av", "aux", "ext"};
    size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (parts & (1u << i)) n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s%s", n ? "," : "", names[i]);
    if (!out[0]) snprintf(out, cap, "none");
    return out;
}

int replay_session_first_divergence(uint32_t *frame, unsigned *parts)
{
    if (!s_diverged) return 0;
    if (frame) *frame = s_div_frame;
    if (parts) *parts = s_div_parts;
    return 1;
}

unsigned replay_session_digests_checked(void) { return s_digests_checked; }

/* ---- Recording ---- */

static void free_recording(void)
{
    free(s_thumb); s_thumb = NULL; s_have_thumb = 0;
    free(s_digests); s_digests = NULL; s_digest_count = 0;
    free(s_words); s_words = NULL;
    free(s_anchor); s_anchor = NULL; s_anchor_size = 0;
    free(s_last_ram); s_last_ram = NULL;
    free(s_cards); s_cards = NULL; s_cards_mask = 0;
    s_power_on = 0;
    s_product[0] = 0;
    s_frames = s_steps = 0;
    s_stop_requested = 0;
}

/* The file name of a path, for the toast. */
static const char *path_leaf(const char *path)
{
    const char *leaf = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    return leaf;
}

static int refuse_record(const char *why)
{
    char msg[320];
    snprintf(msg, sizeof msg, "Replay not recorded: %s", why);
    replay_host_osd(msg, 2200);
    return 0;
}

/* An anchored recording arms here and starts at the boundary after its anchor
 * settles; a power-on recording starts at this boundary. */
static int begin_recording(const char *path, int slot, int power_on)
{
    char why[256] = "";
    if (s_state != REPLAY_IDLE) return 0;
    if (power_on && !replay_host_at_power_on())
        return refuse_record("a replay from power-on must start at boot");
    if (!replay_host_can_record(why, sizeof why)) return refuse_record(why);
    memset(&s_meta, 0, sizeof s_meta);
    if (!replay_host_identity(&s_meta, why, sizeof why)) return refuse_record(why);
    s_words = (InputRouteDualShockWord *)malloc(INPUT_ROUTE_MAX_FRAMES * sizeof *s_words);
    s_last_ram = (uint8_t *)malloc(RAM_BYTES);
    s_digests = (uint32_t (*)[5])malloc(DIGEST_CAP * sizeof *s_digests);
    s_digest_count = 0;
    s_thumb = (uint32_t *)malloc(REPLAY_THUMB_W * REPLAY_THUMB_H * sizeof *s_thumb);
    s_have_thumb = 0;
    if (power_on) s_cards = (uint8_t *)calloc(2, INPUT_ROUTE_REPLAY_CARD_BYTES);
    if (!s_words || !s_last_ram || !s_digests || !s_thumb || (power_on && !s_cards)) {
        free_recording();
        return refuse_record("out of memory");
    }
    if (power_on && !replay_host_cards_capture(s_cards, &s_cards_mask)) {
        free_recording();
        return refuse_record("the memory cards cannot be read");
    }
    if (!power_on && !replay_host_request_anchor()) {
        free_recording();
        return refuse_record("out of memory");
    }
    replay_host_settings_capture(s_settings, sizeof s_settings);
    snprintf(s_rec_path, sizeof s_rec_path, "%s", path);
    s_rec_slot = slot;
    s_result = REPLAY_RESULT_NONE;
    replay_host_product(s_product, sizeof s_product);
    if (!power_on) {
        s_state = REPLAY_ARMING;
        return 1;
    }
    replay_host_power_on_begin();
    s_power_on = 1;
    s_state = REPLAY_RECORDING;
    s_frames = s_steps = 0;
    s_rec_started = time(NULL);
    replay_host_osd("Recording replay from power-on", 1600);
    fprintf(stdout, "replay_recording: path=%s power_on=1 cards=%u\n", s_rec_path, (unsigned)s_cards_mask);
    fflush(stdout);
    return 1;
}

int replay_session_record_to(const char *path)
{
    if (!path || !path[0] || strlen(path) >= PATH_BYTES || file_exists(path)) return 0;
    return begin_recording(path, -1, 0);
}

int replay_session_record_power_on(const char *path)
{
    if (!path || !path[0] || strlen(path) >= PATH_BYTES || file_exists(path)) return 0;
    return begin_recording(path, -1, 1);
}

/* "<game> · m:ss · YYYY-MM-DD HH:MM", cut to fit on a UTF-8 boundary. */
static void default_name(char *out, size_t cap)
{
    char when[32] = "", tail[64];
    struct tm tm_start;
    const int rate = replay_host_frame_rate() > 0 ? replay_host_frame_rate() : 60;
    const unsigned secs = (unsigned)(s_last_frame / (uint32_t)rate);
    const char *title = replay_host_game_title();
    size_t n;
#ifdef _WIN32
    localtime_s(&tm_start, &s_rec_started);
#else
    localtime_r(&s_rec_started, &tm_start);
#endif
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm_start);
    snprintf(tail, sizeof tail, " \xC2\xB7 %u:%02u \xC2\xB7 %s", secs / 60u, secs % 60u, when);
    if (!title || !title[0]) title = "Replay";
    n = strlen(title);
    if (cap <= strlen(tail) + 1) { snprintf(out, cap, "%s", tail + 1); return; }
    if (n > cap - strlen(tail) - 1) {
        n = cap - strlen(tail) - 1;
        while (n && ((unsigned char)title[n] & 0xC0) == 0x80) --n;   /* keep whole characters */
    }
    snprintf(out, cap, "%.*s%s", (int)n, title, tail);
}

/* Writes the recording that ends on boundary s_last_frame to the new file
 * `path` and reads it back with the reader playback uses. Returns NULL, or
 * the reason; a file this call created is removed on failure. */
static const char *write_recording(const char *path)
{
    const char *error = NULL;
    InputRouteV3 meta = s_meta;
    InputRouteMarker end_marker;
    InputRouteCheckpoint *end = (InputRouteCheckpoint *)malloc(sizeof *end);
    unsigned char *digests = NULL;
    uint32_t digests_length = 0;
    FILE *f = NULL;
    if (!end) error = "out of memory";
    if (!error) {
        checkpoint_of(s_last_frame, s_last_ram, s_last_cycle, end);
        end_marker.frame = s_last_frame;
        end_marker.kind = INPUT_ROUTE_MARKER_END;
        meta.frames = s_last_frame;
        meta.record_size = INPUT_DUALSHOCK_ROUTE_RECORD_BYTES;
        meta.marker_count = meta.checkpoint_count = 1;
        if (s_digest_count) {
            digests_length = 4u + s_digest_count * INPUT_ROUTE_REPLAY_DIGEST_BYTES;
            if (!(digests = (unsigned char *)malloc(digests_length))) error = "out of memory";
            else {
                input_route_put32(digests, s_digest_count);
                for (uint32_t i = 0; i < s_digest_count; ++i)
                    for (uint32_t k = 0; k < 5; ++k)
                        input_route_put32(digests + 4 + i * INPUT_ROUTE_REPLAY_DIGEST_BYTES + 4 * k, s_digests[i][k]);
            }
        }
        if (!error && !create_exclusive(path, &f)) error = "the file already exists or cannot be created";
    }
    if (!error) {
        InputRouteV3ReplayOut rx;
        char name[REPLAY_NAME_MAX + 1];
        default_name(name, sizeof name);
        memset(&rx, 0, sizeof rx);
        rx.anchor = s_anchor;
        rx.anchor_length = (uint32_t)s_anchor_size;
        rx.settings = s_settings;
        rx.digests = digests;
        rx.digests_length = digests_length;
        if (s_have_thumb) { rx.thumb = s_thumb; rx.thumb_w = REPLAY_THUMB_W; rx.thumb_h = REPLAY_THUMB_H; }
        rx.name = name;
        rx.product = s_product[0] ? s_product : NULL;
        if (s_power_on) {
            /* Only the inserted cards' images, in slot order. */
            rx.power_on = 1;
            rx.cards_mask = s_cards_mask;
            rx.cards = s_cards_mask == 2u ? s_cards + INPUT_ROUTE_REPLAY_CARD_BYTES : s_cards;
        }
        error = input_route_v3_write_ex(f, &meta, NULL, s_words, s_last_frame, &end_marker, end, &rx);
        if (fclose(f) && !error) error = "close error";
        if (error) remove(path);
    }
    if (!error) {
        InputRouteV3 back;
        InputRouteV3Replay *rp = (InputRouteV3Replay *)malloc(sizeof *rp);
        InputDualShockRouteStep *steps = (InputDualShockRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
        InputRouteMarker *markers = (InputRouteMarker *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *markers);
        InputRouteCheckpoint *cps = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
        FILE *r = fopen(path, "rb");
        error = !rp || !steps || !markers || !cps || !r ? "cannot reopen the replay"
              : input_route_v3_read_ex(r, &back, NULL, steps, markers, cps, rp);
        if (r) fclose(r);
        if (!error && (back.frames != s_last_frame || rp->anchor_length != s_anchor_size ||
                       rp->digest_count != s_digest_count || rp->power_on != s_power_on ||
                       rp->cards_mask != s_cards_mask))
            error = "the replay does not read back identically";
        free(rp); free(steps); free(markers); free(cps);
        if (error) remove(path);
    }
    free(end);
    free(digests);
    return error;
}

/* Power-on recording: replace the crash-recovery copy with one that ends on
 * this boundary. The old copy is kept until the new one reads back. */
static void write_partial(void)
{
    char partial[PATH_BYTES], tmp[PATH_BYTES + 8];
    const char *error;
    if (!replay_session_partial_path(s_rec_path, partial, sizeof partial)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", partial);
    remove(tmp);
    error = write_recording(tmp);
    if (!error) {
        remove(partial);
        if (rename(tmp, partial)) { remove(tmp); error = "rename failed"; }
    }
    if (error) fprintf(stderr, "replay: partial copy not written: %s (%s)\n", error, partial);
    else fprintf(stdout, "replay_partial: path=%s frames=%u\n", partial, (unsigned)s_last_frame);
    fflush(stdout);
}

/* Writes the recording that ended on boundary s_last_frame. */
static void finish_recording(void)
{
    char msg[PATH_BYTES + 64], partial[PATH_BYTES];
    const char *error;
    s_state = REPLAY_IDLE;
    if (!s_frames) {
        replay_host_osd("Replay not saved: nothing was recorded", 2000);
        free_recording();
        return;
    }
    error = write_recording(s_rec_path);
    if (error) {
        snprintf(msg, sizeof msg, "Replay not saved: %s", error);
        fprintf(stderr, "replay: %s (%s)\n", msg, s_rec_path);
    } else {
        /* The finished replay supersedes the crash-recovery copy. */
        if (s_power_on && replay_session_partial_path(s_rec_path, partial, sizeof partial))
            remove(partial);
        if (s_rec_slot >= 0) snprintf(msg, sizeof msg, "Replay saved: slot %d", s_rec_slot + 1);
        else {
            /* The file's name without folder or extension: the longest boot
             * name then fits the toast. The full path is in the log line. */
            const char *leaf = path_leaf(s_rec_path);
            size_t n = strlen(leaf);
            if (n > 7 && !strcmp(leaf + n - 7, ".psxrpl")) n -= 7;
            snprintf(msg, sizeof msg, "Replay saved: %.*s", (int)n, leaf);
        }
        fprintf(stdout, "replay_recorded: path=%s frames=%u steps=%u\n", s_rec_path,
                (unsigned)s_last_frame, (unsigned)s_steps);
        fflush(stdout);
    }
    replay_host_osd(msg, 2200);
    free_recording();
}

int replay_session_toggle_record(void)
{
    if (s_state == REPLAY_ARMING || s_state == REPLAY_RECORDING) {
        s_stop_requested = 1;
        return 1;
    }
    if (s_state != REPLAY_IDLE) {
        replay_host_osd("Replay playing: take over to record", 1800);
        return 0;
    }
    int slot = replay_session_next_free_slot();
    char path[PATH_BYTES];
    if (slot < 0) {
        replay_host_osd("No free replay slots", 2200);
        return 0;
    }
    if (!replay_session_slot_path(slot, path, sizeof path)) {
        replay_host_osd("Replay not recorded: save states are not available", 2200);
        return 0;
    }
    return begin_recording(path, slot, 0);
}

static void record_boundary(uint16_t b, const uint8_t sticks[4])
{
    /* This boundary closes frame s_frames: keep its RAM for the END checkpoint. */
    s_last_frame = s_frames;
    s_last_cycle = replay_host_cycle();
    memcpy(s_last_ram, replay_host_ram(), RAM_BYTES);
    if (s_frames % REPLAY_DIGEST_INTERVAL == 0 && s_digest_count < DIGEST_CAP) {
        uint32_t d[4];
        if (replay_host_state_digest(d)) {
            s_digests[s_digest_count][0] = s_frames;
            memcpy(&s_digests[s_digest_count][1], d, sizeof d);
            s_digest_count++;
        }
    }
    if (s_stop_requested) { finish_recording(); return; }
    if (s_power_on && s_frames && s_frames % REPLAY_PARTIAL_INTERVAL == 0) write_partial();
    InputRouteDualShockWord w;
    w.buttons = b;
    /* File order LY,LX,RY,RX; host order LX,LY,RX,RY. */
    w.axes_ly_lx_ry_rx[0] = sticks[1]; w.axes_ly_lx_ry_rx[1] = sticks[0];
    w.axes_ly_lx_ry_rx[2] = sticks[3]; w.axes_ly_lx_ry_rx[3] = sticks[2];
    const int new_step = !s_frames || s_words[s_frames - 1].buttons != w.buttons ||
                         memcmp(s_words[s_frames - 1].axes_ly_lx_ry_rx, w.axes_ly_lx_ry_rx, 4);
    if (s_frames == INPUT_ROUTE_MAX_FRAMES || (new_step && s_steps == INPUT_ROUTE_MAX_STEPS)) {
        replay_host_osd("Replay stopped: length limit reached", 2200);
        finish_recording();
        return;
    }
    s_steps += (uint32_t)new_step;
    s_words[s_frames++] = w;
}

/* ---- Playback ---- */

static void end_playback(ReplayResult result, const char *osd)
{
    write_verdict(result, osd);
    if (s_settings_switched) replay_host_settings_restore();
    s_settings_switched = 0;
    if (s_cards_installed) replay_host_cards_restore();
    s_cards_installed = 0;
    free(s_steps_play); s_steps_play = NULL;
    free(s_play_digests); s_play_digests = NULL;
    s_play_digest_count = s_play_digest_next = 0;
    s_state = REPLAY_IDLE;
    s_result = result;
    if (osd) replay_host_osd(osd, 2400);
}

static int same_text_ci(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if ((*a | 0x20) != (*b | 0x20)) return 0;
    return *a == *b;
}

static int refuse_play(const char *why)
{
    char msg[320];
    snprintf(msg, sizeof msg, "Replay not played: %s", why);
    fprintf(stderr, "replay: %s\n", msg);
    replay_host_osd(msg, 2600);
    s_result = REPLAY_RESULT_FAILED;
    write_verdict(REPLAY_RESULT_FAILED, msg);
    return 0;
}

/* The first value of `key`, or "" when absent; return its declaration count. */
static unsigned line_value(const char *lines, const char *key, char *out, size_t cap)
{
    const size_t k = strlen(key);
    unsigned count = 0;
    out[0] = 0;
    for (const char *p = lines; p && *p; ) {
        const char *end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n > k && !strncmp(p, key, k) && p[k] == '=') {
            if (!count++) snprintf(out, cap, "%.*s", (int)(n - k - 1), p + k + 1);
        }
        p = end ? end + 1 : p + n;
    }
    return count;
}

int replay_session_play_file(const char *path)
{
    InputRouteV3 meta, product;
    InputRouteV3Replay *rp = NULL;
    InputRouteMarker *markers = NULL;
    InputRouteCheckpoint *cps = NULL;
    InputDualShockRouteStep *steps = NULL;
    uint8_t *anchor = NULL, *cards = NULL;
    unsigned char *dig = NULL;
    uint32_t (*digests)[5] = NULL;
    const char *error = NULL;
    char why[256] = "", player[INPUT_ROUTE_REPLAY_PRODUCT_MAX + 1] = "";
    char rec_crc[16], player_crc[16];
    FILE *f;
    if (s_state != REPLAY_IDLE) return refuse_play("a replay is already recording or playing");
    snprintf(s_play_path, sizeof s_play_path, "%s", path ? path : "");
    s_rec_pin[0] = s_player_pin[0] = 0;
    s_rec_exe[0] = s_player_exe[0] = 0;
    s_rec_platform[0] = s_player_platform[0] = 0;
    s_rec_codegen[0] = s_player_codegen[0] = 0;
    s_play_core_digest_version = 3u;
    s_cross_platform = 0;
    s_play_power_on = 0;
    s_play_frame = s_play_frames = 0;
    s_diverged = 0;
    s_digests_checked = 0;
    s_end_reached = 0;
    if (!(f = fopen(path, "rb"))) return refuse_play("cannot open the file");
    rp = (InputRouteV3Replay *)malloc(sizeof *rp);
    markers = (InputRouteMarker *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *markers);
    cps = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
    steps = (InputDualShockRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
    error = !rp || !markers || !cps || !steps ? "out of memory"
          : input_route_v3_read_ex(f, &meta, NULL, steps, markers, cps, rp);
    if (!error && rp->power_on) {
        /* The inserted cards' images, back into their slots. */
        s_play_power_on = 1;
        if (!replay_host_at_power_on())
            error = "it starts at power-on: play it from startup (--replay FILE)";
        else if (!(cards = (uint8_t *)calloc(2, INPUT_ROUTE_REPLAY_CARD_BYTES)))
            error = "out of memory";
        else if (fseek(f, rp->cards_offset, SEEK_SET))
            error = "cannot read the memory cards";
        for (uint32_t c = 0; !error && c < 2; ++c)
            if ((rp->cards_mask >> c & 1u) &&
                fread(cards + c * INPUT_ROUTE_REPLAY_CARD_BYTES, 1, INPUT_ROUTE_REPLAY_CARD_BYTES, f) !=
                    INPUT_ROUTE_REPLAY_CARD_BYTES)
                error = "cannot read the memory cards";
    } else if (!error) {
        if (!(anchor = (uint8_t *)malloc(rp->anchor_length))) error = "out of memory";
        else if (fseek(f, rp->anchor_offset, SEEK_SET) ||
                 fread(anchor, 1, rp->anchor_length, f) != rp->anchor_length)
            error = "cannot read the anchor";
    }
    if (!error && rp->digest_count) {
        const uint32_t n = rp->digest_length - 4u;
        dig = (unsigned char *)malloc(n);
        digests = (uint32_t (*)[5])malloc(rp->digest_count * sizeof *digests);
        if (!dig || !digests) error = "out of memory";
        else if (fseek(f, rp->digest_offset + 4, SEEK_SET) || fread(dig, 1, n, f) != n)
            error = "cannot read the state digests";
        for (uint32_t i = 0; !error && i < rp->digest_count; ++i) {
            for (uint32_t k = 0; k < 5; ++k)
                digests[i][k] = input_route_le32(dig + i * INPUT_ROUTE_REPLAY_DIGEST_BYTES + 4 * k);
            if (digests[i][0] > meta.frames || (i && digests[i][0] <= digests[i - 1][0]))
                error = "state digest order";
        }
    }
    free(dig);
    fclose(f);
    memset(&product, 0, sizeof product);
    if (!error && !replay_host_identity(&product, why, sizeof why)) error = why;
    if (!error && (strcmp(meta.disc_serial, product.disc_serial) ||
                   meta.disc_digest_kind != product.disc_digest_kind ||
                   memcmp(meta.disc_digest, product.disc_digest, 32)))
        error = "it was recorded on a different game or disc image";
    /* The product lines name the exe and the BIOS image by hash: a stem match
     * with another BIOS dump, or a Workbench build with no pin (PS1B-287),
     * still shows up here. */
    if (!error) {
        replay_host_product(player, sizeof player);
        line_value(rp->product, "exe_sha256", s_rec_exe, sizeof s_rec_exe);
        line_value(player, "exe_sha256", s_player_exe, sizeof s_player_exe);
        line_value(rp->product, "bios_crc32", rec_crc, sizeof rec_crc);
        line_value(player, "bios_crc32", player_crc, sizeof player_crc);
        line_value(rp->product, "platform", s_rec_platform, sizeof s_rec_platform);
        line_value(player, "platform", s_player_platform, sizeof s_player_platform);
        line_value(rp->product, "codegen", s_rec_codegen, sizeof s_rec_codegen);
        line_value(player, "codegen", s_player_codegen, sizeof s_player_codegen);
        char digest_version[16];
        unsigned digest_declarations = line_value(rp->product, "core_digest", digest_version, sizeof digest_version);
        if (!digest_declarations)
            s_play_core_digest_version = 1u;
        else if (digest_declarations != 1u || !digest_version[0])
            error = "empty or repeated core digest version";
        else if (strcmp(digest_version, "1") == 0)
            s_play_core_digest_version = 1u;
        else if (strcmp(digest_version, "2") == 0)
            s_play_core_digest_version = 2u;
        else if (strcmp(digest_version, "3") != 0)
            error = "unsupported core digest version";
    }
    /* The BIOS image decides, not its file name: with a CRC on both sides the
     * CRC is the test, so the same dump under another name (another machine's
     * naming) plays. Without both CRCs the file-name stem is all there is. */
    if (!error && (rec_crc[0] && player_crc[0] ? !same_text_ci(rec_crc, player_crc)
                                               : !same_text_ci(meta.bios_stem, product.bios_stem)))
        error = "it was recorded with a different BIOS";
    if (!error && strcmp(meta.boot_mode, product.boot_mode))
        error = "it was recorded with a different boot mode";
    if (error) {
        char e[256];
        snprintf(e, sizeof e, "%s", error);
        free(rp); free(markers); free(cps); free(steps); free(anchor); free(cards); free(digests);
        return refuse_play(e);
    }
    const int other_pin = strcmp(meta.pin, product.pin) != 0;
    int other_build = other_pin ||
                      (s_rec_exe[0] && s_player_exe[0] && strcmp(s_rec_exe, s_player_exe));
    /* The same pin built for another platform has another exe by construction;
     * it is the same build, not a different one. */
    s_cross_platform = !other_pin && s_rec_platform[0] && s_player_platform[0] &&
                       strcmp(s_rec_platform, s_player_platform) != 0;
    snprintf(s_rec_pin, sizeof s_rec_pin, "%s", meta.pin);
    snprintf(s_player_pin, sizeof s_player_pin, "%s", product.pin);
    char differs[512] = "";
    replay_host_settings_apply(rp->settings, differs, sizeof differs);
    s_settings_switched = 1;
    if (s_play_power_on) {
        s_cards_installed = replay_host_cards_install(cards, rp->cards_mask);
        free(cards);
        if (!s_cards_installed) {
            free(rp); free(markers); free(cps); free(steps); free(digests);
            end_playback(REPLAY_RESULT_FAILED, "Replay not played: its memory cards cannot be inserted");
            return 0;
        }
        replay_host_power_on_begin();
    } else if (!replay_host_load_anchor(anchor, rp->anchor_length)) {
        free(rp); free(markers); free(cps); free(steps); free(anchor); free(digests);
        end_playback(REPLAY_RESULT_FAILED, "Replay not played: the anchor state cannot be loaded");
        return 0;
    }
    s_end = cps[meta.checkpoint_count - 1];
    s_steps_play = steps;
    s_play_steps = meta.steps;
    s_play_frames = meta.frames;
    s_play_step = 0;
    s_play_left = s_play_steps ? steps[0].frames : 0;
    s_play_frame = 0;
    s_takeover_armed = 0;
    s_play_digests = digests;
    s_play_digest_count = rp->digest_count;
    s_play_digest_next = 0;
    s_digests_checked = s_div_parts = 0;
    s_div_frame = 0;
    s_diverged = 0;
    s_result = REPLAY_RESULT_NONE;
    /* A power-on replay feeds record 0 at this boundary, as it was recorded. */
    s_state = s_play_power_on ? REPLAY_PLAYING : REPLAY_LOADING;
    if (s_cross_platform) {
        char msg[160];
        snprintf(msg, sizeof msg, "Replay from the same build on another platform (%s)", s_rec_platform);
        replay_host_osd(msg, 2600);
        fprintf(stderr, "replay: recorded on build %s for %s (exe %s), this is the %s build (exe %s)\n",
                meta.pin, s_rec_platform, s_rec_exe[0] ? s_rec_exe : "?", s_player_platform,
                s_player_exe[0] ? s_player_exe : "?");
    } else if (other_build) {
        replay_host_osd("Replay from a different build: it may go out of sync", 2600);
        fprintf(stderr, "replay: recorded on build %s (exe %s), this is %s (exe %s)\n", meta.pin,
                s_rec_exe[0] ? s_rec_exe : "?", product.pin, s_player_exe[0] ? s_player_exe : "?");
    } else if (differs[0]) {
        char msg[640];
        /* Only settings playback cannot switch are listed (the others are
         * switched for playback and restored after, so they are not news). */
        snprintf(msg, sizeof msg, "Replay may go out of sync: different %s", differs);
        replay_host_osd(msg, 2600);
    } else if (s_play_core_digest_version < 3u) {
        replay_host_osd("Replay uses older runtime behavior: it may go out of sync", 2600);
    } else {
        replay_host_osd("Replay playing", 1200);
    }
    if (s_play_core_digest_version < 3u)
        fprintf(stderr, "replay: older core digest v%u; current runtime uses v3\n",
                s_play_core_digest_version);
    fprintf(stdout, "replay_playing: path=%s frames=%u other_build=%d power_on=%d cross_platform=%d\n", path,
            (unsigned)meta.frames, other_build, s_play_power_on, s_cross_platform);
    fflush(stdout);
    free(rp); free(markers); free(cps); free(anchor);
    return 1;
}

int replay_session_play_slot(int slot)
{
    char path[PATH_BYTES];
    if (!replay_session_slot_path(slot, path, sizeof path) || !replay_session_slot_exists(slot))
        return refuse_play("that slot is empty");
    return replay_session_play_file(path);
}

static int live_input(uint16_t buttons, const uint8_t sticks[4])
{
    if (buttons != 0xFFFFu) return 1;
    for (int i = 0; i < 4; ++i) {
        int d = (int)sticks[i] - 0x80;
        if (d > TAKEOVER_DEADZONE || d < -TAKEOVER_DEADZONE) return 1;
    }
    return 0;
}

/* Compares the rollback state digest recorded for this boundary, if any. */
static void check_digest(void)
{
    uint32_t d[4];
    unsigned parts = 0;
    char text[32];
    if (s_play_digest_next >= s_play_digest_count ||
        s_play_digests[s_play_digest_next][0] != s_play_frame) return;
    const uint32_t *want = &s_play_digests[s_play_digest_next++][1];
    if (!replay_host_state_digest(d)) return;
    s_digests_checked++;
    for (unsigned k = 0; k < 4; ++k) parts |= (d[k] != want[k]) << k;
    if (!parts || s_diverged) return;
    s_diverged = 1;
    s_div_frame = s_play_frame;
    s_div_parts = parts;
    fprintf(stdout, "replay_diverged: frame=%u parts=%s core=%08x/%08x av=%08x/%08x aux=%08x/%08x ext=%08x/%08x\n",
            (unsigned)s_play_frame, digest_parts_text(parts, text, sizeof text),
            d[0], want[0], d[1], want[1], d[2], want[2], d[3], want[3]);
    fflush(stdout);
}

static int play_boundary(uint16_t live, const uint8_t live_sticks[4],
                         uint16_t *out_buttons, uint8_t out_sticks[4])
{
    check_digest();
    if (s_play_frame == s_play_frames) {
        InputRouteCheckpoint now;
        checkpoint_of(s_play_frame, replay_host_ram(), replay_host_cycle(), &now);
        const int ram = !memcmp(now.ram_sha256, s_end.ram_sha256, 32);
        const int cyc = now.cycle == s_end.cycle;
        unsigned differing = 0;
        char text[32];
        for (uint32_t p = 0; p < INPUT_ROUTE_RAM_PAGES; ++p) differing += now.pages[p] != s_end.pages[p];
        /* AV alone does not fail a replay (see REPLAY_DIGEST_AV). */
        const int state = !(s_diverged && (s_div_parts & ~REPLAY_DIGEST_AV));
        s_end_reached = 1;
        s_end_cycle = now.cycle;
        s_end_recorded_cycle = s_end.cycle;
        s_end_pages = differing;
        fprintf(stdout, "replay_end: frames=%u result=%s cycle=%llu recorded_cycle=%llu differing_pages=%u "
                "digests_checked=%u first_divergence=%d divergence_parts=%s\n",
                (unsigned)s_play_frame, ram && cyc && state ? "in_sync" : "out_of_sync",
                (unsigned long long)now.cycle, (unsigned long long)s_end.cycle, differing,
                s_digests_checked, s_diverged ? (int)s_div_frame : -1,
                digest_parts_text(s_diverged ? s_div_parts : 0, text, sizeof text));
        fflush(stdout);
        if (ram && cyc && state) {
            end_playback(REPLAY_RESULT_IN_SYNC, "Replay finished (in sync)");
        } else {
            char msg[160];
            snprintf(msg, sizeof msg, "Replay finished (out of sync: %u RAM pages, cycle %+lld)",
                     differing, (long long)(now.cycle - s_end.cycle));
            end_playback(REPLAY_RESULT_OUT_OF_SYNC, msg);
        }
        return 0;
    }
    /* Any live input takes over, once the pad has been neutral after the start
     * (the press that started playback does not count). */
    if (live_input(live, live_sticks)) {
        if (s_takeover_armed) {
            end_playback(REPLAY_RESULT_TAKEN_OVER, "Playback stopped: you have control");
            return 0;
        }
    } else {
        s_takeover_armed = 1;
    }
    const InputDualShockRouteStep *s = &s_steps_play[s_play_step];
    *out_buttons = s->buttons;
    out_sticks[0] = s->axes_ly_lx_ry_rx[1]; out_sticks[1] = s->axes_ly_lx_ry_rx[0];
    out_sticks[2] = s->axes_ly_lx_ry_rx[3]; out_sticks[3] = s->axes_ly_lx_ry_rx[2];
    s_play_frame++;
    if (--s_play_left == 0 && ++s_play_step < s_play_steps) s_play_left = s_steps_play[s_play_step].frames;
    return 1;
}

int replay_session_boundary(uint16_t live, const uint8_t live_sticks[4],
                            uint16_t *out_buttons, uint8_t out_sticks[4])
{
    switch (s_state) {
    case REPLAY_IDLE:
        return 0;
    case REPLAY_ARMING: {
        uint8_t *data = NULL;
        size_t size = 0;
        const int r = replay_host_take_anchor(&data, &size);
        if (r == 0) return 0;
        if (r < 0 || !data || !size || size > INPUT_ROUTE_V3_MAX_EXT / 2) {
            free(data);
            free_recording();
            s_state = REPLAY_IDLE;
            replay_host_osd("Replay not recorded: the anchor state could not be saved", 2400);
            return 0;
        }
        s_anchor = data;
        s_anchor_size = size;
        if (s_stop_requested) {           /* F11 twice before the anchor settled */
            free_recording();
            s_state = REPLAY_IDLE;
            replay_host_osd("Replay cancelled", 1200);
            return 0;
        }
        s_state = REPLAY_RECORDING;
        s_frames = s_steps = 0;
        s_have_thumb = s_thumb && replay_host_thumb(s_thumb);
        s_rec_started = time(NULL);
        replay_host_osd("Recording replay", 1200);
        fprintf(stdout, "replay_recording: path=%s\n", s_rec_path);
        fflush(stdout);
    }   /* fall through: boundary 0 records its input */
    /* FALLTHROUGH */
    case REPLAY_RECORDING:
        record_boundary(live, live_sticks);
        if (s_state != REPLAY_RECORDING) return 0;
        *out_buttons = live;
        memcpy(out_sticks, live_sticks, 4);
        return 1;
    case REPLAY_LOADING: {
        const int r = replay_host_take_load_result();
        if (r == 0) return 0;
        if (r < 0) {
            end_playback(REPLAY_RESULT_FAILED,
                         "Replay not played: its anchor state does not load in this build");
            return 0;
        }
        s_state = REPLAY_PLAYING;
        return play_boundary(live, live_sticks, out_buttons, out_sticks);
    }
    case REPLAY_PLAYING:
        return play_boundary(live, live_sticks, out_buttons, out_sticks);
    }
    return 0;
}

void replay_session_shutdown(void)
{
    if (s_state == REPLAY_RECORDING) {
        finish_recording();
    } else if (s_state == REPLAY_ARMING) {
        free_recording();
        s_state = REPLAY_IDLE;
    } else if (s_state == REPLAY_LOADING || s_state == REPLAY_PLAYING) {
        end_playback(REPLAY_RESULT_NONE, NULL);
    }
}
