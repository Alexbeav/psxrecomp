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
/* Rollback state digests: frame, core, av, aux, ext. */
#define DIGEST_CAP (INPUT_ROUTE_MAX_FRAMES / REPLAY_DIGEST_INTERVAL + 2u)
static uint32_t (*s_digests)[5];
static uint32_t s_digest_count;

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

int replay_session_export_slot(int slot, char *out_path, size_t cap)
{
    char src[PATH_BYTES], dst[PATH_BYTES];
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
    free(s_digests); s_digests = NULL; s_digest_count = 0;
    free(s_words); s_words = NULL;
    free(s_anchor); s_anchor = NULL; s_anchor_size = 0;
    free(s_last_ram); s_last_ram = NULL;
    s_frames = s_steps = 0;
    s_stop_requested = 0;
}

static int begin_recording(const char *path, int slot)
{
    char why[256] = "";
    if (s_state != REPLAY_IDLE) return 0;
    if (!replay_host_can_record(why, sizeof why)) {
        char msg[320];
        snprintf(msg, sizeof msg, "Replay not recorded: %s", why);
        replay_host_osd(msg, 2200);
        return 0;
    }
    memset(&s_meta, 0, sizeof s_meta);
    if (!replay_host_identity(&s_meta, why, sizeof why)) {
        char msg[320];
        snprintf(msg, sizeof msg, "Replay not recorded: %s", why);
        replay_host_osd(msg, 2200);
        return 0;
    }
    s_words = (InputRouteDualShockWord *)malloc(INPUT_ROUTE_MAX_FRAMES * sizeof *s_words);
    s_last_ram = (uint8_t *)malloc(RAM_BYTES);
    s_digests = (uint32_t (*)[5])malloc(DIGEST_CAP * sizeof *s_digests);
    s_digest_count = 0;
    if (!s_words || !s_last_ram || !s_digests || !replay_host_request_anchor()) {
        free_recording();
        replay_host_osd("Replay not recorded: out of memory", 2200);
        return 0;
    }
    replay_host_settings_capture(s_settings, sizeof s_settings);
    snprintf(s_rec_path, sizeof s_rec_path, "%s", path);
    s_rec_slot = slot;
    s_result = REPLAY_RESULT_NONE;
    s_state = REPLAY_ARMING;
    return 1;
}

int replay_session_record_to(const char *path)
{
    FILE *probe;
    if (!path || !path[0] || strlen(path) >= PATH_BYTES) return 0;
    if ((probe = fopen(path, "rb"))) { fclose(probe); return 0; }
    return begin_recording(path, -1);
}

/* Writes the recording that ended on boundary s_last_frame. */
static void finish_recording(void)
{
    char msg[PATH_BYTES + 64];
    const char *error = NULL;
    InputRouteMarker end_marker;
    InputRouteCheckpoint *end = NULL;
    unsigned char *digests = NULL;
    uint32_t digests_length = 0;
    FILE *f = NULL;
    s_state = REPLAY_IDLE;
    if (!s_frames) {
        replay_host_osd("Replay not saved: nothing was recorded", 2000);
        free_recording();
        return;
    }
    end = (InputRouteCheckpoint *)malloc(sizeof *end);
    if (!end) error = "out of memory";
    if (!error) {
        checkpoint_of(s_last_frame, s_last_ram, s_last_cycle, end);
        end_marker.frame = s_last_frame;
        end_marker.kind = INPUT_ROUTE_MARKER_END;
        s_meta.frames = s_last_frame;
        s_meta.record_size = INPUT_DUALSHOCK_ROUTE_RECORD_BYTES;
        s_meta.marker_count = s_meta.checkpoint_count = 1;
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
        if (!error && !create_exclusive(s_rec_path, &f)) error = "the file already exists or cannot be created";
    }
    if (!error) {
        error = input_route_v3_write_ex(f, &s_meta, NULL, s_words, s_last_frame, &end_marker, end,
                                        s_anchor, (uint32_t)s_anchor_size, s_settings,
                                        digests, digests_length);
        if (fclose(f) && !error) error = "close error";
        if (error) remove(s_rec_path);
    }
    if (!error) {
        /* The written file must pass the reader playback uses. */
        InputRouteV3 meta;
        InputRouteV3Replay rp;
        InputDualShockRouteStep *steps = (InputDualShockRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
        InputRouteMarker *markers = (InputRouteMarker *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *markers);
        InputRouteCheckpoint *cps = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
        FILE *r = fopen(s_rec_path, "rb");
        error = !steps || !markers || !cps || !r ? "cannot reopen the replay"
              : input_route_v3_read_ex(r, &meta, NULL, steps, markers, cps, &rp);
        if (r) fclose(r);
        if (!error && (meta.frames != s_last_frame || rp.anchor_length != s_anchor_size ||
                       rp.digest_count != s_digest_count))
            error = "the replay does not read back identically";
        free(steps); free(markers); free(cps);
        if (error) remove(s_rec_path);
    }
    free(end);
    free(digests);
    if (error) {
        snprintf(msg, sizeof msg, "Replay not saved: %s", error);
        fprintf(stderr, "replay: %s (%s)\n", msg, s_rec_path);
    } else {
        if (s_rec_slot >= 0) snprintf(msg, sizeof msg, "Replay saved: slot %d", s_rec_slot + 1);
        else snprintf(msg, sizeof msg, "Replay saved: %s", s_rec_path);
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
    return begin_recording(path, slot);
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
    if (s_settings_switched) replay_host_settings_restore();
    s_settings_switched = 0;
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
    return 0;
}

int replay_session_play_file(const char *path)
{
    InputRouteV3 meta, product;
    InputRouteV3Replay *rp = NULL;
    InputRouteMarker *markers = NULL;
    InputRouteCheckpoint *cps = NULL;
    InputDualShockRouteStep *steps = NULL;
    uint8_t *anchor = NULL;
    unsigned char *dig = NULL;
    uint32_t (*digests)[5] = NULL;
    const char *error = NULL;
    char why[256] = "";
    FILE *f;
    if (s_state != REPLAY_IDLE) return refuse_play("a replay is already recording or playing");
    if (!(f = fopen(path, "rb"))) return refuse_play("cannot open the file");
    rp = (InputRouteV3Replay *)malloc(sizeof *rp);
    markers = (InputRouteMarker *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *markers);
    cps = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
    steps = (InputDualShockRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
    error = !rp || !markers || !cps || !steps ? "out of memory"
          : input_route_v3_read_ex(f, &meta, NULL, steps, markers, cps, rp);
    if (!error && !(anchor = (uint8_t *)malloc(rp->anchor_length))) error = "out of memory";
    if (!error && (fseek(f, rp->anchor_offset, SEEK_SET) ||
                   fread(anchor, 1, rp->anchor_length, f) != rp->anchor_length))
        error = "cannot read the anchor";
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
    if (!error && !same_text_ci(meta.bios_stem, product.bios_stem))
        error = "it was recorded with a different BIOS";
    if (!error && strcmp(meta.boot_mode, product.boot_mode))
        error = "it was recorded with a different boot mode";
    if (error) {
        char e[256];
        snprintf(e, sizeof e, "%s", error);
        free(rp); free(markers); free(cps); free(steps); free(anchor); free(digests);
        return refuse_play(e);
    }
    int other_build = strcmp(meta.pin, product.pin) != 0;
    char differs[512] = "";
    replay_host_settings_apply(rp->settings, differs, sizeof differs);
    s_settings_switched = 1;
    if (!replay_host_load_anchor(anchor, rp->anchor_length)) {
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
    s_state = REPLAY_LOADING;
    if (other_build) {
        replay_host_osd("Replay from a different build: it may go out of sync", 2600);
        fprintf(stderr, "replay: recorded on build %s, this is %s\n", meta.pin, product.pin);
    } else if (differs[0]) {
        char msg[640];
        snprintf(msg, sizeof msg, "Replay settings differ: %s", differs);
        replay_host_osd(msg, 2600);
    } else {
        replay_host_osd("Replay playing", 1200);
    }
    fprintf(stdout, "replay_playing: path=%s frames=%u other_build=%d\n", path, (unsigned)meta.frames, other_build);
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
