/* PS1B-191 replay recorder and player: the session state machine against a
 * small deterministic stand-in machine behind the replay_host_* hooks.
 *
 * The stand-in guest folds each frame's P1 input into RAM and advances its
 * cycle count, so a replay that feeds the recorded input from the anchor must
 * end on the recorded RAM hash and cycle. */
#include "replay_session.h"
#include "input_route_v3_file.h"
#include <stdarg.h>
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
static int drift;                  /* perturb the guest during playback */
static uint32_t vram_word;         /* stand-in for state outside RAM (the AV digest) */
static int drift_av;               /* perturb only that during playback */

/* ---- host stubs ---- */
static char osd_last[256];
static int osd_count;
void replay_host_osd(const char *text, int ms) { (void)ms; snprintf(osd_last, sizeof osd_last, "%s", text); osd_count++; }
static int can_record = 1;
int replay_host_can_record(char *why, size_t cap) { if (!can_record) snprintf(why, cap, "netplay"); return can_record; }
static char product_pin[41] = "0123456789abcdef0123456789abcdef01234567";
static char product_serial[16] = "SLUS-00001";
int replay_host_identity(InputRouteV3 *m, char *why, size_t cap) {
    (void)why; (void)cap;
    m->has_identity = 1;
    snprintf(m->pin, sizeof m->pin, "%s", product_pin);
    snprintf(m->disc_serial, sizeof m->disc_serial, "%s", product_serial);
    snprintf(m->bios_stem, sizeof m->bios_stem, "SCPH1001");
    snprintf(m->boot_mode, sizeof m->boot_mode, "hle");
    m->disc_digest_kind = INPUT_ROUTE_DISC_DIGEST_FILE;
    memset(m->disc_digest, 0xAB, 32);
    return 1;
}
/* The anchor blob is the whole stand-in machine: RAM then cycle. */
static int anchor_pending, anchor_fail;
static uint8_t *anchor_blob; static size_t anchor_size;
int replay_host_request_anchor(void) { anchor_pending = 1; return 1; }
int replay_host_take_anchor(uint8_t **data, size_t *size) {
    if (anchor_pending != 2) return 0;
    anchor_pending = 0;
    if (anchor_fail) return -1;
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
static char settings_now[256] = "cd_speed=1\n", settings_saved[256], settings_applied[256];
static int restores;
static char stub_differs[64];      /* settings the host could not switch */
void replay_host_settings_capture(char *out, size_t cap) { snprintf(out, cap, "%s", settings_now); }
void replay_host_settings_apply(const char *s, char *differs, size_t cap) {
    snprintf(settings_saved, sizeof settings_saved, "%s", settings_now);
    snprintf(settings_applied, sizeof settings_applied, "%s", s);
    snprintf(settings_now, sizeof settings_now, "%s", s);
    snprintf(differs, cap, "%s", stub_differs);
}
void replay_host_settings_restore(void) { snprintf(settings_now, sizeof settings_now, "%s", settings_saved); restores++; }
const uint8_t *replay_host_ram(void) { return ram; }
static int digest_calls;
int replay_host_state_digest(uint32_t out[4]) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < RAM_BYTES; ++i) h = (h ^ ram[i]) * 16777619u;   /* all of RAM, like the core digest */
    for (unsigned i = 0; i < 8; ++i) h = (h ^ (uint8_t)(cycle >> (8 * i))) * 16777619u;
    out[0] = h; out[1] = vram_word; out[2] = 0xA0A0u; out[3] = 0xE0E0u;
    digest_calls++;
    return 1;
}
uint64_t replay_host_cycle(void) { return cycle; }
static char dir[512];
int replay_host_slot_base(char *d, size_t dc, char *p, size_t pc) { snprintf(d, dc, "%s", dir); snprintf(p, pc, "replay_80010000"); return 1; }
const char *replay_host_export_dir(void) { static char e[600]; snprintf(e, sizeof e, "%s/replays", dir); return e; }
const char *replay_host_disc_serial(void) { return product_serial; }

/* ---- the stand-in machine ---- */
/* The host's safe boundary: a pending anchor saves the machine and reloads
 * it; a pending load restores it. */
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
static void run_frame(uint16_t buttons, const uint8_t st[4]) {
    safe_point();
    uint32_t h = (uint32_t)(cycle * 2654435761u) ^ buttons ^ ((uint32_t)st[0] << 16) ^ ((uint32_t)st[3] << 24);
    for (unsigned i = 0; i < 64; ++i) ram[(h + i * 4099u) % RAM_BYTES] ^= (uint8_t)(h >> (i % 24));
    if (drift && replay_session_state() == REPLAY_PLAYING) ram[12345]++;
    if (drift_av && replay_session_state() == REPLAY_PLAYING) vram_word++;
    cycle += 564480u + (buttons & 7u);
}
/* One vblank: the boundary decides P1, then the guest runs a frame. */
static void vblank(uint16_t live, const uint8_t live_st[4]) {
    uint16_t b; uint8_t st[4];
    if (!replay_session_boundary(live, live_st, &b, st)) { b = live; memcpy(st, live_st, 4); }
    run_frame(b, st);
}
static const uint8_t neutral[4] = { 0x80, 0x80, 0x80, 0x80 };
static uint16_t script(unsigned i) { return (uint16_t)~(1u << (i % 16)); }

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void slot_path(int s, char *out, size_t cap) { CHECK(replay_session_slot_path(s, out, cap), "slot path %d", s); }
static long file_size(const char *p) { FILE *f = fopen(p, "rb"); long n = -1; if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); } return n; }
static void clear_slots(void) { char p[700]; for (int s = 0; s < REPLAY_SLOTS; ++s) { slot_path(s, p, sizeof p); remove(p); } }

/* Record `n` frames of scripted input into the next free slot. */
static int record(unsigned n, const char *settings) {
    snprintf(settings_now, sizeof settings_now, "%s", settings);
    int free_slot = replay_session_next_free_slot();
    CHECK(replay_session_toggle_record(), "record start");
    CHECK(replay_session_state() == REPLAY_ARMING, "arming after F11");
    vblank(0xFFFF, neutral);                       /* anchor saved + reloaded in this frame */
    CHECK(replay_session_state() == REPLAY_ARMING, "still arming until the next boundary");
    for (unsigned i = 0; i < n; ++i) {
        uint8_t st[4] = { (uint8_t)(0x80 + i), 0x80, 0x80, (uint8_t)(0x7F - i) };
        vblank(script(i), st);                     /* boundary 0 starts the recording */
        if (!i) CHECK(replay_session_state() == REPLAY_RECORDING && replay_session_owns_p1(),
                      "recording from the first boundary after the anchor");
    }
    CHECK(replay_session_toggle_record(), "record stop");
    vblank(0xFFFF, neutral);                       /* END checkpoint, file written */
    CHECK(replay_session_state() == REPLAY_IDLE, "idle after stop");
    return free_slot;
}

static void test_record_and_play_in_sync(void) {
    clear_slots();
    memset(ram, 0, sizeof ram); cycle = 1000;
    for (unsigned i = 0; i < 30; ++i) vblank(script(i), neutral);   /* mid-game */
    int slot = record(120, "cd_speed=2\n");
    CHECK(slot == 0, "first free slot is 0 (got %d)", slot);
    CHECK(replay_session_slot_exists(0), "slot 0 written");
    char p[700]; slot_path(0, p, sizeof p);
    /* The replay reader admits it; the plain route reader refuses it. */
    FILE *f = fopen(p, "rb");
    InputRouteV3 meta; InputRouteV3Replay rp;
    InputDualShockRouteStep *steps = calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
    InputRouteMarker markers[INPUT_ROUTE_V3_MAX_MARKERS];
    InputRouteCheckpoint *cps = calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
    const char *err = f ? input_route_v3_read_ex(f, &meta, NULL, steps, markers, cps, &rp) : "open";
    CHECK(!err, "replay reader: %s", err ? err : "");
    CHECK(!err && meta.frames == 120 && meta.record_size == INPUT_DUALSHOCK_ROUTE_RECORD_BYTES, "120 DualShock frames");
    CHECK(!err && rp.has_anchor && rp.anchor_length == RAM_BYTES + 8, "anchor embedded");
    CHECK(!err && !strcmp(rp.settings, "cd_speed=2\n"), "settings embedded: %s", rp.settings);
    CHECK(!err && markers[meta.marker_count - 1].kind == INPUT_ROUTE_MARKER_END, "END marker");
    /* A rollback state digest every REPLAY_DIGEST_INTERVAL frames: 0, 60, 120. */
    CHECK(!err && rp.digest_count == 3 && rp.digest_length == 4 + 3 * 20,
          "state digests embedded (count %u)", err ? 0u : rp.digest_count);
    if (f) { fseek(f, 0, SEEK_SET); err = input_route_v3_read(f, &meta, NULL, steps, markers, cps); fclose(f); }
    CHECK(err && !strcmp(err, "unsupported mandatory extension tag"), "plain route reader refuses a replay: %s", err ? err : "accepted");
    free(steps); free(cps);

    /* Play on: the machine moved on, settings went back to the user's. */
    for (unsigned i = 0; i < 50; ++i) vblank(0xFFFF, neutral);
    snprintf(settings_now, sizeof settings_now, "cd_speed=1\n");
    restores = 0;
    CHECK(replay_session_play_slot(0), "play slot 0");
    CHECK(!strcmp(settings_applied, "cd_speed=2\n"), "recorded settings switched on");
    CHECK(replay_session_state() == REPLAY_LOADING, "loading");
    vblank(0xFFFF, neutral);                       /* anchor loaded in this frame */
    vblank(0xFFFF, neutral);                       /* boundary 0 feeds record 0 */
    CHECK(replay_session_state() == REPLAY_PLAYING, "playing");
    unsigned frames = 1;
    while (replay_session_state() != REPLAY_IDLE && frames < 200) { vblank(0xFFFF, neutral); frames++; }
    CHECK(frames == 121, "120 records then the END boundary (took %u)", frames);
    CHECK(replay_session_last_result() == REPLAY_RESULT_IN_SYNC, "in sync (result %d, osd %s)", replay_session_last_result(), osd_last);
    CHECK(restores == 1 && !strcmp(settings_now, "cd_speed=1\n"), "settings restored");
    uint32_t df = 0; unsigned parts = 0;
    CHECK(!replay_session_first_divergence(&df, &parts), "no digest divergence in sync");
    CHECK(replay_session_digests_checked() == 3, "3 digests checked (got %u)", replay_session_digests_checked());
}

static void test_out_of_sync_is_reported(void) {
    drift = 1;
    CHECK(replay_session_play_slot(0), "play slot 0 again");
    for (unsigned i = 0; i < 130 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    drift = 0;
    CHECK(replay_session_last_result() == REPLAY_RESULT_OUT_OF_SYNC, "drift reported (result %d)", replay_session_last_result());
    CHECK(strstr(osd_last, "out of sync") != NULL, "OSD says out of sync: %s", osd_last);
    /* The first digest after the drift names the frame and the core partition. */
    uint32_t df = 0; unsigned parts = 0;
    CHECK(replay_session_first_divergence(&df, &parts) && df == 60 && (parts & REPLAY_DIGEST_CORE),
          "first divergence at frame 60 in core (frame %u parts %u)", df, parts);
    /* State outside the verdict partitions (the AV digest: GPU + VRAM, which
     * forks on GL/Vulkan readback) is reported but does not fail the replay. */
    drift_av = 1;
    CHECK(replay_session_play_slot(0), "play slot 0 with AV drift");
    for (unsigned i = 0; i < 130 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    drift_av = 0;
    CHECK(replay_session_last_result() == REPLAY_RESULT_IN_SYNC, "AV-only drift stays in sync (result %d)", replay_session_last_result());
    CHECK(replay_session_first_divergence(&df, &parts) && df == 60 && parts == REPLAY_DIGEST_AV,
          "AV divergence reported (frame %u parts %u)", df, parts);
}

static void test_take_over(void) {
    restores = 0;
    CHECK(replay_session_play_slot(0), "play for take-over");
    vblank(0xFFFF, neutral);
    vblank(0xFFFF, neutral);
    CHECK(replay_session_state() == REPLAY_PLAYING, "playing before take-over");
    uint16_t b; uint8_t st[4];
    const uint8_t pushed[4] = { 0xFF, 0x80, 0x80, 0x80 };
    int driven = replay_session_boundary(0xFFFF, pushed, &b, st);   /* stick past the deadzone */
    CHECK(!driven && replay_session_state() == REPLAY_IDLE, "stick takes over");
    CHECK(replay_session_last_result() == REPLAY_RESULT_TAKEN_OVER && restores == 1, "take-over result and restore");
    /* A button held when playback starts does not take over until released. */
    CHECK(replay_session_play_slot(0), "play again");
    vblank(0xFFBF, neutral);
    driven = replay_session_boundary(0xFFBF, neutral, &b, st);
    CHECK(driven && replay_session_state() == REPLAY_PLAYING, "held button before neutral does not take over");
    run_frame(b, st);
    vblank(0xFFFF, neutral);
    driven = replay_session_boundary(0xFFBF, neutral, &b, st);
    CHECK(!driven && replay_session_last_result() == REPLAY_RESULT_TAKEN_OVER, "a fresh press takes over");
}

static void test_no_free_slots(void) {
    clear_slots();
    char p[700];
    for (int s = 0; s < REPLAY_SLOTS; ++s) { slot_path(s, p, sizeof p); FILE *f = fopen(p, "wb"); fputs("keep", f); fclose(f); }
    CHECK(replay_session_next_free_slot() == -1, "no free slot");
    osd_last[0] = 0;
    CHECK(!replay_session_toggle_record(), "F11 refused when full");
    CHECK(replay_session_state() == REPLAY_IDLE && strstr(osd_last, "No free replay slots"), "message: %s", osd_last);
    for (int s = 0; s < REPLAY_SLOTS; ++s) { slot_path(s, p, sizeof p); CHECK(file_size(p) == 4, "slot %d untouched", s); }
    CHECK(replay_session_delete_slot(5) && !replay_session_slot_exists(5), "delete slot 5");
    CHECK(replay_session_next_free_slot() == 5, "slot 5 is free again");
    clear_slots();
}

static void test_identity(void) {
    clear_slots();
    record(20, "cd_speed=1\n");
    /* Another build: plays with a warning. */
    snprintf(product_pin, sizeof product_pin, "fedcba9876543210fedcba9876543210fedcba98");
    osd_last[0] = 0;
    CHECK(replay_session_play_slot(0), "other build still plays");
    CHECK(strstr(osd_last, "different build") != NULL, "build warning: %s", osd_last);
    for (unsigned i = 0; i < 30 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    snprintf(product_pin, sizeof product_pin, "0123456789abcdef0123456789abcdef01234567");
    /* Settings the host switches say nothing; one it cannot switch is named. */
    osd_last[0] = 0;
    CHECK(replay_session_play_slot(0) && !strcmp(osd_last, "Replay playing"), "switched settings: plain start (%s)", osd_last);
    for (unsigned i = 0; i < 30 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    snprintf(stub_differs, sizeof stub_differs, "enabled mods");
    CHECK(replay_session_play_slot(0), "plays with different mods");
    CHECK(strstr(osd_last, "may go out of sync") && strstr(osd_last, "enabled mods"), "mods warning: %s", osd_last);
    for (unsigned i = 0; i < 30 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    stub_differs[0] = 0;
    /* Another game: refused. */
    snprintf(product_serial, sizeof product_serial, "SLUS-00002");
    CHECK(!replay_session_play_slot(0), "another disc is refused");
    CHECK(replay_session_state() == REPLAY_IDLE && strstr(osd_last, "different game"), "refusal: %s", osd_last);
    snprintf(product_serial, sizeof product_serial, "SLUS-00001");
    /* Recording refused when the host says so; anchor failure returns to idle. */
    can_record = 0;
    CHECK(!replay_session_toggle_record() && replay_session_state() == REPLAY_IDLE, "can_record=0 refuses");
    can_record = 1;
    anchor_fail = 1;
    CHECK(replay_session_toggle_record(), "start with a failing anchor");
    vblank(0xFFFF, neutral);                       /* the save fails in this frame */
    vblank(0xFFFF, neutral);                       /* reported at the next boundary */
    CHECK(replay_session_state() == REPLAY_IDLE && strstr(osd_last, "anchor"), "anchor failure: %s", osd_last);
    anchor_fail = 0;
}

static void test_shutdown_while_recording(void) {
    clear_slots();
    CHECK(replay_session_toggle_record(), "start");
    vblank(0xFFFF, neutral);
    for (unsigned i = 0; i < 40; ++i) vblank(script(i), neutral);
    replay_session_shutdown();                     /* e.g. the window was closed */
    CHECK(replay_session_slot_exists(0) && replay_session_state() == REPLAY_IDLE, "shutdown writes the replay");
    CHECK(replay_session_play_slot(0), "the shutdown replay plays");
    for (unsigned i = 0; i < 50 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    CHECK(replay_session_last_result() == REPLAY_RESULT_IN_SYNC, "and ends in sync (result %d)", replay_session_last_result());
}

static void test_rec_blink(void) {
    CHECK(!replay_session_rec_visible(0), "no REC while idle");
    clear_slots();
    replay_session_toggle_record(); vblank(0xFFFF, neutral);
    CHECK(replay_session_rec_visible(100) && !replay_session_rec_visible(600) && replay_session_rec_visible(1100), "1 Hz blink");
    replay_session_toggle_record(); vblank(0xFFFF, neutral);
    clear_slots();
}

int main(int argc, char **argv) {
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "replay_session_test_dir");
    mkdir_p(dir);
    test_record_and_play_in_sync();
    test_out_of_sync_is_reported();
    test_take_over();
    test_no_free_slots();
    test_identity();
    test_shutdown_while_recording();
    test_rec_blink();
    clear_slots();
    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: replay session, %d checks\n", checks);
    return 0;
}
