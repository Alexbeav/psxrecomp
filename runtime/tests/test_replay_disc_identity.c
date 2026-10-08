/* PS1B-316: a replay names the disc in the drive, not the disc the game was
 * launched on.
 *
 * The real product identity (input_route_session.c) sits behind the replay
 * session here, with two small files standing in for the discs of a set. A
 * launch is input_route_session_set_product; a disc mounted under the running
 * game (an in-game disc change, or a save state that mounted its own disc) is
 * input_route_session_set_disc, which is what the host's
 * replay_identity_follow_disc calls.
 *
 * The case: launch on disc 1, change to disc 2, record with F11. The replay
 * must name disc 2, so that
 *   - it plays in the same session,
 *   - a later launch on disc 1 refuses it,
 *   - a later launch on disc 2 plays it. */
#include "replay_session.h"
#include "input_route_session.h"
#include "input_route_v3_file.h"
#include "disc_digest_cache.h"
#include "sio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_p(d) _mkdir(d)
#else
#include <sys/stat.h>
#define mkdir_p(d) mkdir(d, 0755)
#endif

#define RAM_BYTES (2u << 20)
static uint8_t ram[RAM_BYTES];
static uint64_t cycle;

/* ---- what input_route_session.c links against ---- */
uint8_t *g_psx_ram = ram;
uint64_t psx_cycle_count;
void sio_set_multitap(int enabled) { (void)enabled; }
void sio_set_pad_connected(int slot, int value) { (void)slot; (void)value; }
void sio_set_pad_config_capable(int slot, int value) { (void)slot; (void)value; }
void sio_set_pad_analog(int slot, int enabled, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{ (void)slot; (void)enabled; (void)a; (void)b; (void)c; (void)d; }
void sio_set_pad_state_slot(int slot, uint16_t buttons) { (void)slot; (void)buttons; }
void sio_set_pad_sticks(int slot, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
{ (void)slot; (void)lx; (void)ly; (void)rx; (void)ry; }

/* ---- replay host stubs ---- */
static char osd_last[256];
void replay_host_osd(const char *text, int ms) { (void)ms; snprintf(osd_last, sizeof osd_last, "%s", text); }
int replay_host_capture(const char *path) { (void)path; return 0; }
int replay_host_can_record(char *why, size_t cap) { (void)why; (void)cap; return 1; }
/* The host's hook, as main.cpp has it: the real product identity. */
int replay_host_identity(InputRouteV3 *m, char *why, size_t cap) {
    return input_route_session_identity(m, /*call_hle*/1, /*boot_skip*/0, why, cap);
}
/* The anchor blob is the whole stand-in machine: RAM then cycle. It holds no
 * disc name, like the save state inside a real replay. */
static int anchor_pending;
static uint8_t *anchor_blob; static size_t anchor_size;
int replay_host_request_anchor(void) { anchor_pending = 1; return 1; }
int replay_host_take_anchor(uint8_t **data, size_t *size) {
    if (anchor_pending != 2) return 0;
    anchor_pending = 0;
    *data = anchor_blob; *size = anchor_size; anchor_blob = NULL;
    return 1;
}
static int load_pending; static uint8_t *load_blob; static size_t load_size;
int replay_host_load_anchor(const void *d, size_t n) {
    load_blob = realloc(load_blob, n); memcpy(load_blob, d, n); load_size = n; load_pending = 1; return 1;
}
int replay_host_take_load_result(void) {
    if (load_pending != 2) return 0;
    load_pending = 0;
    return load_size == RAM_BYTES + 8 ? 1 : -1;
}
void replay_host_settings_capture(char *out, size_t cap) { snprintf(out, cap, "cd_speed=1\n"); }
void replay_host_settings_apply(const char *s, char *differs, size_t cap) { (void)s; if (cap) differs[0] = 0; }
void replay_host_settings_restore(void) {}
const uint8_t *replay_host_ram(void) { return ram; }
int replay_host_thumb(uint32_t *out) { memset(out, 0, REPLAY_THUMB_W * REPLAY_THUMB_H * sizeof *out); return 1; }
const char *replay_host_game_title(void) { return "Two Disc Game"; }
int replay_host_frame_rate(void) { return 60; }
int replay_host_state_digest(uint32_t out[4]) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < RAM_BYTES; ++i) h = (h ^ ram[i]) * 16777619u;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ (uint8_t)(cycle >> (8 * i))) * 16777619u;
    out[0] = h; out[1] = 0; out[2] = 0xA0A0u; out[3] = 0xE0E0u;
    return 1;
}
uint64_t replay_host_cycle(void) { return cycle; }
static char dir[512];
int replay_host_slot_base(char *d, size_t dc, char *p, size_t pc) { snprintf(d, dc, "%s", dir); snprintf(p, pc, "replay_80010000"); return 1; }
const char *replay_host_export_dir(void) { static char e[600]; snprintf(e, sizeof e, "%s/replays", dir); return e; }
const char *replay_host_disc_serial(void) { return "replay"; }
int replay_host_at_power_on(void) { return 0; }
void replay_host_power_on_begin(void) {}
int replay_host_cards_capture(uint8_t *images, uint32_t *mask) { (void)images; *mask = 0; return 1; }
int replay_host_cards_install(const uint8_t *images, uint32_t mask) { (void)images; (void)mask; return 1; }
void replay_host_cards_restore(void) {}
void replay_host_product(char *out, size_t cap) {
    snprintf(out, cap, "exe_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                       "codegen=12345678\nbios_crc32=1234abcd\nrenderer=software\nplatform=test\n");
}

/* ---- the stand-in machine (as test_replay_session.c) ---- */
static void safe_point(void) {
    if (anchor_pending == 1) {
        anchor_size = RAM_BYTES + 8;
        anchor_blob = malloc(anchor_size);
        memcpy(anchor_blob, ram, RAM_BYTES); memcpy(anchor_blob + RAM_BYTES, &cycle, 8);
        anchor_pending = 2;
    }
    if (load_pending == 1) {
        if (load_size == RAM_BYTES + 8) { memcpy(ram, load_blob, RAM_BYTES); memcpy(&cycle, load_blob + RAM_BYTES, 8); }
        load_pending = 2;
    }
}
static const uint8_t neutral[4] = { 0x80, 0x80, 0x80, 0x80 };
static void vblank(uint16_t live) {
    uint16_t b; uint8_t st[4];
    if (!replay_session_boundary(live, neutral, &b, st)) { b = live; memcpy(st, neutral, 4); }
    safe_point();
    uint32_t h = (uint32_t)(cycle * 2654435761u) ^ b;
    for (unsigned i = 0; i < 64; ++i) ram[(h + i * 4099u) % RAM_BYTES] ^= (uint8_t)(h >> (i % 24));
    cycle += 564480u + (b & 7u);
}
static uint16_t script(unsigned i) { return (uint16_t)~(1u << (i % 16)); }

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- the discs and the host's two identity calls ---- */
#define FRAMES 90u
static char disc1[700], disc2[700], disc2_other[700];
static const char bios[] = "bios/SCPH1001.BIN";
static uint8_t sha_disc1[32], sha_disc2[32];

static void write_disc(const char *path, unsigned seed) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "write %s", path);
    for (unsigned i = 0; f && i < 150000u; ++i) fputc((int)((i * seed + (i >> 7)) & 0xFF), f);
    if (f) fclose(f);
}
/* A process start on this disc. */
static void launch(const char *serial, const char *disc) {
    replay_session_shutdown();
    input_route_session_set_product(serial, disc, bios);
    memset(ram, 0, sizeof ram); cycle = 1000;
    for (unsigned i = 0; i < 30; ++i) vblank(script(i));
}
/* A disc mounted under the running game. */
static void mount(const char *serial, const char *disc) {
    CHECK(replay_session_state() == REPLAY_IDLE, "the host mounts a disc only while no replay runs");
    input_route_session_set_disc(serial, disc);
}

/* F11 replay of FRAMES frames to `path`. */
static void record(const char *path) {
    remove(path);
    CHECK(replay_session_record_to(path), "record starts (%s)", osd_last);
    vblank(0xFFFF);                                /* anchor saved + reloaded */
    for (unsigned i = 0; i < FRAMES; ++i) vblank(script(i));
    CHECK(replay_session_state() == REPLAY_RECORDING, "recording");
    CHECK(replay_session_toggle_record(), "record stop");
    vblank(0xFFFF);                                /* END checkpoint, file written */
    CHECK(replay_session_state() == REPLAY_IDLE, "idle after stop");
}
static int header(const char *path, InputRouteV3 *meta) {
    FILE *f = fopen(path, "rb");
    InputRouteV3Replay *rp = malloc(sizeof *rp);
    InputDualShockRouteStep *steps = calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
    InputRouteMarker *markers = calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *markers);
    InputRouteCheckpoint *cps = calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
    const char *err = f ? input_route_v3_read_ex(f, meta, NULL, steps, markers, cps, rp) : "open";
    int anchored = !err && rp->has_anchor && !rp->power_on;
    CHECK(!err, "replay reads back: %s", err ? err : "");
    if (f) fclose(f);
    free(rp); free(steps); free(markers); free(cps);
    return anchored;
}
/* Plays `path`; REPLAY_RESULT_NONE when it was refused (the reason in osd_last). */
static ReplayResult play(const char *path) {
    osd_last[0] = 0;
    for (unsigned i = 0; i < 20; ++i) vblank(0xFFFF);   /* the machine moved on */
    if (!replay_session_play_file(path)) return REPLAY_RESULT_NONE;
    for (unsigned i = 0; i < FRAMES + 10u && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF);
    return replay_session_last_result();
}

int main(int argc, char **argv) {
    char after_change[700], before_change[700], back_on_one[700], verdict[700];
    InputRouteV3 meta;
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "replay_disc_identity_dir");
    mkdir_p(dir);
    snprintf(disc1, sizeof disc1, "%s/Game (Disc 1).bin", dir);
    snprintf(disc2, sizeof disc2, "%s/Game (Disc 2).bin", dir);
    snprintf(disc2_other, sizeof disc2_other, "%s/Game (Disc 2) other dump.bin", dir);
    snprintf(after_change, sizeof after_change, "%s/after-change.psxrpl", dir);
    snprintf(before_change, sizeof before_change, "%s/before-change.psxrpl", dir);
    snprintf(back_on_one, sizeof back_on_one, "%s/back-on-one.psxrpl", dir);
    snprintf(verdict, sizeof verdict, "%s/verdict.json", dir);
    replay_session_set_verdict_path(verdict);
    write_disc(disc1, 31u); write_disc(disc2, 77u); write_disc(disc2_other, 131u);
    CHECK(disc_digest_full_sha256(disc1, sha_disc1) && disc_digest_full_sha256(disc2, sha_disc2) &&
          memcmp(sha_disc1, sha_disc2, 32), "two different disc images");

    /* Launch on disc 1. A replay recorded now names disc 1. */
    launch("SLUS-00001", disc1);
    CHECK(input_route_session_prefetch_disc_digest(), "the launch disc is hashed (and cached)");
    record(before_change);
    CHECK(header(before_change, &meta), "anchored replay");
    CHECK(!strcmp(meta.disc_serial, "SLUS-00001") && !memcmp(meta.disc_digest, sha_disc1, 32),
          "before any change the replay names the launch disc");

    /* Change to disc 2, then record. The header names disc 2: serial and
     * digest. The cached digest of disc 1 is not reused. */
    mount("SLUS-00002", disc2);
    record(after_change);
    CHECK(header(after_change, &meta), "anchored replay");
    CHECK(!strcmp(meta.disc_serial, "SLUS-00002"), "the replay names the mounted disc's serial (got %s)", meta.disc_serial);
    CHECK(meta.disc_digest_kind == INPUT_ROUTE_DISC_DIGEST_FILE && !memcmp(meta.disc_digest, sha_disc2, 32),
          "the replay holds the mounted disc's digest");
    CHECK(!strcmp(meta.bios_stem, "SCPH1001") && !strcmp(meta.boot_mode, "hle-calls"),
          "the BIOS and boot mode stay (bios %s, boot %s)", meta.bios_stem, meta.boot_mode);

    /* 1. The same session, disc 2 still in the drive: it plays. */
    CHECK(play(after_change) == REPLAY_RESULT_IN_SYNC, "same session: in sync (%s)", osd_last);
    /* The disc-1 replay does not play onto disc 2. */
    CHECK(play(before_change) == REPLAY_RESULT_NONE && strstr(osd_last, "different game or disc image"),
          "a launch-disc replay is refused while disc 2 is mounted (%s)", osd_last);

    /* 2. A later launch on disc 1: refused, not played against the wrong disc. */
    launch("SLUS-00001", disc1);
    CHECK(play(after_change) == REPLAY_RESULT_NONE && strstr(osd_last, "different game or disc image"),
          "later launch on the wrong disc: refused (%s)", osd_last);
    CHECK(replay_session_state() == REPLAY_IDLE, "nothing started");
    CHECK(play(before_change) == REPLAY_RESULT_IN_SYNC, "the disc-1 replay plays on disc 1 (%s)", osd_last);

    /* 3. A later launch on disc 2: it plays. */
    launch("SLUS-00002", disc2);
    CHECK(play(after_change) == REPLAY_RESULT_IN_SYNC, "later launch on the right disc: in sync (%s)", osd_last);

    /* The image decides, not the serial alone: another dump of disc 2. */
    launch("SLUS-00002", disc2_other);
    CHECK(play(after_change) == REPLAY_RESULT_NONE && strstr(osd_last, "different game or disc image"),
          "same serial, another image: refused (%s)", osd_last);
    /* A set whose discs share one serial is told apart by the image. */
    launch("SLUS-00001", disc1);
    mount("SLUS-00001", disc2);
    CHECK(play(before_change) == REPLAY_RESULT_NONE, "one serial for both discs: the digest still refuses (%s)", osd_last);

    /* The identity follows every mount: back to disc 1 in the same session
     * (what a save state taken on disc 1 does when it loads). */
    launch("SLUS-00001", disc1);
    mount("SLUS-00002", disc2);
    mount("SLUS-00001", disc1);
    record(back_on_one);
    CHECK(header(back_on_one, &meta), "anchored replay");
    CHECK(!strcmp(meta.disc_serial, "SLUS-00001") && !memcmp(meta.disc_digest, sha_disc1, 32),
          "after a change back the replay names disc 1 again");
    CHECK(play(back_on_one) == REPLAY_RESULT_IN_SYNC, "and plays there (%s)", osd_last);

    /* A disc without a boot serial has no identity; nothing records. */
    mount("", disc2);
    remove(after_change);
    osd_last[0] = 0;
    CHECK(!replay_session_record_to(after_change) && strstr(osd_last, "boot serial") &&
          replay_session_state() == REPLAY_IDLE,
          "no serial on the mounted disc: recording refused (%s)", osd_last);
    /* A mounted image that cannot be read refuses too, with the reason. */
    mount("SLUS-00002", "no/such/disc.bin");
    osd_last[0] = 0;
    CHECK(!replay_session_record_to(after_change) && strstr(osd_last, "cannot read the disc image"),
          "unreadable image: recording refused (%s)", osd_last);

    replay_session_shutdown();
    remove(after_change); remove(before_change); remove(back_on_one); remove(verdict);
    remove(disc1); remove(disc2); remove(disc2_other);
    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: replay disc identity, %d checks\n", checks);
    return 0;
}
