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
static char product_bios_stem[32] = "SCPH1001";
static char product_boot_mode[16] = "hle";
static uint8_t product_disc_byte = 0xAB;
int replay_host_identity(InputRouteV3 *m, char *why, size_t cap) {
    (void)why; (void)cap;
    m->has_identity = 1;
    snprintf(m->pin, sizeof m->pin, "%s", product_pin);
    snprintf(m->disc_serial, sizeof m->disc_serial, "%s", product_serial);
    snprintf(m->bios_stem, sizeof m->bios_stem, "%s", product_bios_stem);
    snprintf(m->boot_mode, sizeof m->boot_mode, "%s", product_boot_mode);
    m->disc_digest_kind = INPUT_ROUTE_DISC_DIGEST_FILE;
    memset(m->disc_digest, product_disc_byte, 32);
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
int replay_host_thumb(uint32_t *out) {
    for (unsigned i = 0; i < REPLAY_THUMB_W * REPLAY_THUMB_H; ++i) out[i] = 0xFF000000u | (i * 2654435761u >> 8);
    return 1;
}
const char *replay_host_game_title(void) { return "Test Game"; }
int replay_host_frame_rate(void) { return 60; }
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
/* PS1B-316 power-on replays. The host's cards are what a cold boot finds;
 * the guest reads whichever cards are in (see run_frame). */
static int power_on_now;
int replay_host_at_power_on(void) { return power_on_now; }
static int power_on_begins;
void replay_host_power_on_begin(void) { power_on_begins++; }
#define CARD_BYTES INPUT_ROUTE_REPLAY_CARD_BYTES
static uint8_t host_cards[2][CARD_BYTES];
static uint32_t host_cards_mask = 1;
static uint8_t replay_cards[2][CARD_BYTES];
static uint32_t replay_cards_mask;
static int replay_cards_in;
int replay_host_cards_capture(uint8_t *images, uint32_t *mask) {
    memcpy(images, host_cards, sizeof host_cards); *mask = host_cards_mask; return 1;
}
int replay_host_cards_install(const uint8_t *images, uint32_t mask) {
    memcpy(replay_cards, images, sizeof replay_cards); replay_cards_mask = mask; replay_cards_in = 1; return 1;
}
void replay_host_cards_restore(void) { replay_cards_in = 0; }
static char product_lines[256] =
    "exe_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
    "codegen=12345678\nbios_crc32=1234abcd\nrenderer=software\n";
void replay_host_product(char *out, size_t cap) { snprintf(out, cap, "%s", product_lines); }

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
    /* The guest reads its memory cards: the replay's while they are in. */
    for (unsigned c = 0; c < 2; ++c) {
        const uint32_t mask = replay_cards_in ? replay_cards_mask : host_cards_mask;
        const uint8_t *card = replay_cards_in ? replay_cards[c] : host_cards[c];
        ram[777 + c] = (uint8_t)(ram[777 + c] * 31u + ((mask >> c & 1u) ? card[h % CARD_BYTES] : 0x5Au));
    }
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

static char verdict_path[700];
static char verdict[4096];
static int read_verdict(void) {
    FILE *f = fopen(verdict_path, "rb");
    size_t n = 0;
    verdict[0] = 0;
    if (f) { n = fread(verdict, 1, sizeof verdict - 1, f); fclose(f); }
    verdict[n] = 0;
    remove(verdict_path);
    return f != NULL;
}

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
    CHECK(read_verdict(), "verdict written at the end of playback");
    CHECK(strstr(verdict, "\"result\": \"in_sync\"") && strstr(verdict, "\"frames_played\": 120") &&
          strstr(verdict, "\"frames_total\": 120") && strstr(verdict, "\"first_divergence_frame\": null") &&
          strstr(verdict, "\"recorded_build\": \"0123456789abcdef0123456789abcdef01234567\"") &&
          strstr(verdict, "\"player_build\": \"0123456789abcdef0123456789abcdef01234567\""),
          "in-sync verdict: %s", verdict);
    CHECK(replay_session_digests_checked() == 3, "3 digests checked (got %u)", replay_session_digests_checked());
}

static void test_thumb_and_name(void) {
    char name[REPLAY_NAME_MAX + 1], expect[64], exported[700];
    static uint32_t thumb[REPLAY_THUMB_W * REPLAY_THUMB_H];
    CHECK(replay_session_slot_info(0, name, sizeof name, thumb), "slot 0 info");
    CHECK(thumb[0] == 0xFF000000u && thumb[5] == (0xFF000000u | (5u * 2654435761u >> 8)), "thumbnail stored");
    snprintf(expect, sizeof expect, "Test Game \xC2\xB7 0:02 \xC2\xB7 ");
    CHECK(!strncmp(name, expect, strlen(expect)) && strlen(name) == strlen(expect) + 16,
          "default name '<game> . m:ss . YYYY-MM-DD HH:MM': %s", name);
    CHECK(!replay_session_rename_slot(0, ""), "empty name refused");
    CHECK(replay_session_rename_slot(0, "Boss fight: take 2"), "rename");
    CHECK(replay_session_slot_info(0, name, sizeof name, NULL) && !strcmp(name, "Boss fight: take 2"), "renamed: %s", name);
    CHECK(replay_session_slot_info(0, NULL, 0, thumb) && thumb[5] == (0xFF000000u | (5u * 2654435761u >> 8)),
          "thumbnail survives the rename");
    CHECK(replay_session_export_slot(0, exported, sizeof exported), "export");
    CHECK(strstr(exported, "/replays/Boss fight_ take 2.psxrpl") != NULL, "export uses the sanitised name: %s", exported);
    remove(exported);
    /* The renamed replay still plays in sync. */
    CHECK(replay_session_play_slot(0), "play after rename");
    for (unsigned i = 0; i < 130 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    CHECK(replay_session_last_result() == REPLAY_RESULT_IN_SYNC, "renamed replay in sync (result %d)", replay_session_last_result());
    read_verdict();
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
    CHECK(read_verdict() && strstr(verdict, "\"result\": \"diverged\"") &&
          strstr(verdict, "\"first_divergence_frame\": 60") && strstr(verdict, "\"divergence_parts\": \"core\""),
          "diverged verdict: %s", verdict);
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
    CHECK(read_verdict() && strstr(verdict, "\"result\": \"stopped_by_input\"") &&
          strstr(verdict, "\"frames_played\": 1,"), "take-over verdict: %s", verdict);
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

/* ---- PS1B-316 power-on replays ---- */

static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    char buf[65536];
    size_t n;
    while (in && out && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    if (in) fclose(in);
    if (out) fclose(out);
}

/* The replay reader's view of a file. */
static const char *read_replay(const char *p, InputRouteV3 *meta, InputRouteV3Replay *rp) {
    InputDualShockRouteStep *steps = calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
    InputRouteMarker markers[INPUT_ROUTE_V3_MAX_MARKERS];
    InputRouteCheckpoint *cps = calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
    FILE *f = fopen(p, "rb");
    const char *err = f ? input_route_v3_read_ex(f, meta, NULL, steps, markers, cps, rp) : "open";
    if (f) fclose(f);
    free(steps); free(cps);
    return err;
}

/* A cold boot of the stand-in machine, with the host's own cards. */
static void cold_boot(void) {
    memset(ram, 0, sizeof ram);
    cycle = 0;
    vram_word = 0;
    for (unsigned i = 0; i < CARD_BYTES; ++i) { host_cards[0][i] = (uint8_t)(i * 7u + 1u); host_cards[1][i] = (uint8_t)(i * 13u + 5u); }
}

/* Plays `p` from a cold boot (vblank 0) to the end; returns the result. With
 * `new_host_cards` the player's own cards differ from the recording's. */
static ReplayResult play_from_boot(const char *p, unsigned max_frames, int new_host_cards) {
    cold_boot();
    if (new_host_cards)
        for (unsigned i = 0; i < CARD_BYTES; ++i) { host_cards[0][i] ^= 0xFF; host_cards[1][i] ^= 0xFF; }
    power_on_now = 1;
    int started = replay_session_play_file(p);
    CHECK(started, "power-on playback starts at boot: %s", osd_last);
    CHECK(!started || replay_session_state() == REPLAY_PLAYING, "no anchor to load: playing at once");
    if (!started) { power_on_now = 0; return replay_session_last_result(); }
    vblank(0xFFFF, neutral);                       /* vblank 0 feeds record 0 */
    power_on_now = 0;
    for (unsigned i = 0; i < max_frames && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xFFFF, neutral);
    return replay_session_last_result();
}

static void test_power_on_record_and_play(void) {
    char path[700], partial[700], crash_copy[700];
    InputRouteV3 meta;
    InputRouteV3Replay *rp = malloc(sizeof *rp);
    const unsigned frames = REPLAY_PARTIAL_INTERVAL + 60u;
    snprintf(path, sizeof path, "%s/Test_Game-boot-19700101T000000Z.psxrpl", dir);
    CHECK(replay_session_partial_path(path, partial, sizeof partial), "partial path");
    snprintf(crash_copy, sizeof crash_copy, "%s/crash-copy.psxrpl", dir);
    remove(path); remove(partial); remove(crash_copy);
    snprintf(settings_now, sizeof settings_now, "cd_speed=1\ncd_game_speed=2\n");

    /* Not at vblank 0: refused, nothing written. */
    cold_boot();
    power_on_now = 0;
    osd_last[0] = 0;
    CHECK(!replay_session_record_power_on(path) && replay_session_state() == REPLAY_IDLE,
          "power-on recording refused after boot");
    CHECK(strstr(osd_last, "must start at boot") != NULL, "refusal says why: %s", osd_last);

    /* Record from vblank 0; the first boundary records record 0. */
    power_on_begins = 0;
    power_on_now = 1;
    CHECK(replay_session_record_power_on(path), "power-on recording starts at boot");
    CHECK(replay_session_state() == REPLAY_RECORDING && replay_session_owns_p1(), "recording at once, no anchor");
    CHECK(power_on_begins == 1, "the host pins overlays for the recording");
    for (unsigned i = 0; i < frames; ++i) {
        uint8_t st[4] = { (uint8_t)(0x80 + i), 0x80, 0x80, 0x80 };
        vblank(script(i / 3u), st);
        power_on_now = 0;
        if (i + 1 == REPLAY_PARTIAL_INTERVAL + 1) {
            /* The boundary after record 1799 wrote the crash-recovery copy. */
            const char *err = read_replay(partial, &meta, rp);
            CHECK(!err, "partial copy reads back: %s", err ? err : "");
            CHECK(!err && meta.frames == REPLAY_PARTIAL_INTERVAL && rp->power_on && !rp->has_anchor &&
                  rp->has_cards && rp->cards_mask == 1u, "partial copy: 1800 frames, power-on, card 1");
            copy_file(partial, crash_copy);        /* what a crash would leave */
        }
    }
    CHECK(!file_exists(path), "nothing final until the recording ends");
    replay_session_shutdown();                     /* the window was closed */
    CHECK(file_exists(path), "exit writes the replay");
    CHECK(!file_exists(partial), "the finished replay removes its partial copy");
    const char *err = read_replay(path, &meta, rp);
    CHECK(!err, "power-on replay reads back: %s", err ? err : "");
    /* Exit ends on the last completed boundary: the last vblank's record
     * never ran, so N vblanks leave N - 1 records. */
    CHECK(!err && meta.frames == frames - 1u && rp->power_on && !rp->has_anchor && rp->has_cards &&
          rp->cards_mask == 1u && !strcmp(rp->settings, "cd_speed=1\ncd_game_speed=2\n") &&
          strstr(rp->product, "bios_crc32=1234abcd") && strstr(rp->product, "exe_sha256=aaaa"),
          "power-on replay: frames, cards, settings, product");
    CHECK(!err && rp->thumb_w == 0, "no thumbnail at power-on (the screen is black)");
    /* The plain route reader refuses it, so it is never replayed as a route. */
    {
        FILE *f = fopen(path, "rb");
        InputDualShockRouteStep *steps = calloc(INPUT_ROUTE_MAX_STEPS, sizeof *steps);
        InputRouteMarker markers[INPUT_ROUTE_V3_MAX_MARKERS];
        InputRouteCheckpoint *cps = calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof *cps);
        err = f ? input_route_v3_read(f, &meta, NULL, steps, markers, cps) : "open";
        if (f) fclose(f);
        free(steps); free(cps);
        CHECK(err && !strcmp(err, "unsupported mandatory extension tag"), "route reader refuses it: %s", err ? err : "accepted");
    }

    /* Play from a cold boot. The host's cards have changed since: the replay
     * brings its own, and puts the host's back afterwards. */
    snprintf(settings_now, sizeof settings_now, "cd_speed=1\ncd_game_speed=1\n");
    restores = 0;
    CHECK(play_from_boot(path, frames + 10u, 1) == REPLAY_RESULT_IN_SYNC, "power-on replay in sync (%s)", osd_last);
    CHECK(!replay_cards_in, "the player's cards are back after playback");
    CHECK(!strcmp(settings_applied, "cd_speed=1\ncd_game_speed=2\n") && restores == 1 &&
          !strcmp(settings_now, "cd_speed=1\ncd_game_speed=1\n"), "recorded settings switched, then restored");
    CHECK(read_verdict() && strstr(verdict, "\"result\": \"in_sync\"") && strstr(verdict, "\"power_on\": true") &&
          strstr(verdict, "\"recorded_exe_sha256\": \"aaaa") && strstr(verdict, "\"player_exe_sha256\": \"aaaa"),
          "power-on verdict: %s", verdict);

    /* What a crash leaves (the partial copy) plays in sync up to its end. */
    CHECK(play_from_boot(crash_copy, frames, 1) == REPLAY_RESULT_IN_SYNC, "the crash copy plays in sync");
    CHECK(read_verdict() && strstr(verdict, "\"frames_played\": 1800,"), "crash copy: 1800 frames: %s", verdict);

    /* Another exe plays with the build warning; another BIOS image is refused. */
    char saved_product[256];
    snprintf(saved_product, sizeof saved_product, "%s", product_lines);
    snprintf(product_lines, sizeof product_lines,
             "exe_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
             "codegen=12345678\nbios_crc32=1234abcd\n");
    cold_boot();
    power_on_now = 1;
    CHECK(replay_session_play_file(path) && strstr(osd_last, "different build"), "another exe: warned (%s)", osd_last);
    replay_session_shutdown();
    CHECK(!replay_cards_in, "ending playback early puts the cards back");
    read_verdict();
    snprintf(product_lines, sizeof product_lines, "%s", saved_product);
    char *crc = strstr(product_lines, "1234abcd");
    memcpy(crc, "99999999", 8);
    CHECK(!replay_session_play_file(path) && strstr(osd_last, "different BIOS"), "another BIOS image: refused (%s)", osd_last);
    read_verdict();
    snprintf(product_lines, sizeof product_lines, "%s", saved_product);
    /* After boot, a power-on replay cannot start. */
    power_on_now = 0;
    CHECK(!replay_session_play_file(path) && strstr(osd_last, "starts at power-on"), "refused after boot (%s)", osd_last);
    read_verdict();
    power_on_now = 1;
    CHECK(!replay_session_record_power_on(path) && replay_session_state() == REPLAY_IDLE,
          "an existing file is never overwritten");
    power_on_now = 0;

    /* Different cards at boot make a replay that ignored them go out of sync:
     * the snapshot is load-bearing. Record with card 2 inserted, then damage
     * the stored image of card 2 in the file. */
    remove(crash_copy);
    host_cards_mask = 3u;
    cold_boot();
    power_on_now = 1;
    CHECK(replay_session_record_power_on(crash_copy), "record with both cards");
    for (unsigned i = 0; i < 120; ++i) { vblank(script(i), neutral); power_on_now = 0; }
    replay_session_toggle_record();
    vblank(0xFFFF, neutral);
    err = read_replay(crash_copy, &meta, rp);
    CHECK(!err && rp->cards_mask == 3u, "both cards stored");
    CHECK(play_from_boot(crash_copy, 200, 1) == REPLAY_RESULT_IN_SYNC, "two-card replay in sync");
    read_verdict();
    if (!err) {
        FILE *f = fopen(crash_copy, "r+b");
        static uint8_t image[CARD_BYTES];
        const long at = rp->cards_offset + (long)CARD_BYTES;   /* card 2's image */
        if (f && !fseek(f, at, SEEK_SET) && fread(image, 1, CARD_BYTES, f) == CARD_BYTES) {
            for (unsigned k = 0; k < CARD_BYTES; ++k) image[k] ^= 0xFF;
            fseek(f, at, SEEK_SET);
            fwrite(image, 1, CARD_BYTES, f);
        }
        if (f) fclose(f);
    }
    CHECK(play_from_boot(crash_copy, 200, 0) == REPLAY_RESULT_OUT_OF_SYNC, "a different card 2 goes out of sync");
    read_verdict();
    host_cards_mask = 1u;
    remove(crash_copy);
    remove(path);
    free(rp);
}

/* What playback refuses, what it only reports, and the cross-platform case
 * (PS1B-316): a replay recorded on the Windows build must keep playing on the
 * Linux and macOS builds of the same pin when the disc file and the BIOS are
 * the same (a thin build reading both from one share). */
static void test_identity_rules(void) {
    char path[700], saved_product[256], saved_pin[41];
    const unsigned frames = 90u;
    InputRouteV3 meta;
    InputRouteV3Replay *rp = malloc(sizeof *rp);
    snprintf(path, sizeof path, "%s/identity-rules.psxrpl", dir);
    remove(path);
    snprintf(saved_product, sizeof saved_product, "%s", product_lines);
    snprintf(saved_pin, sizeof saved_pin, "%s", product_pin);
    snprintf(settings_now, sizeof settings_now, "cd_speed=1\n");

    /* Recorded "on Windows". */
    snprintf(product_lines, sizeof product_lines,
             "exe_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
             "codegen=6a0b6aaa\nbios_crc32=1234abcd\nrenderer=opengl\nplatform=windows-x64\n");
    cold_boot();
    power_on_now = 1;
    CHECK(replay_session_record_power_on(path), "identity: recording starts");
    for (unsigned i = 0; i < frames; ++i) { vblank(script(i / 3u), neutral); power_on_now = 0; }
    replay_session_shutdown();
    const char *err = read_replay(path, &meta, rp);
    CHECK(!err && strstr(rp->product, "platform=windows-x64\n") && strstr(rp->product, "codegen=6a0b6aaa\n") &&
          strstr(rp->product, "bios_crc32=1234abcd\n") && strstr(rp->product, "renderer=opengl\n") &&
          strstr(rp->product, "exe_sha256=aaaa"), "product lines round-trip: %s", err ? err : rp->product);
    CHECK(!err && !strcmp(meta.pin, saved_pin) && !strcmp(meta.disc_serial, "SLUS-00001") &&
          !strcmp(meta.bios_stem, "SCPH1001") && !strcmp(meta.boot_mode, "hle") &&
          meta.disc_digest_kind == INPUT_ROUTE_DISC_DIGEST_FILE && meta.disc_digest[0] == 0xAB &&
          meta.disc_digest[31] == 0xAB, "identity fields round-trip");

    /* The same build: plain start, nothing reported. */
    CHECK(play_from_boot(path, frames + 10u, 0) == REPLAY_RESULT_IN_SYNC, "same build in sync (%s)", osd_last);
    CHECK(read_verdict() && strstr(verdict, "\"cross_platform\": false") &&
          strstr(verdict, "\"recorded_platform\": \"windows-x64\"") &&
          strstr(verdict, "\"player_platform\": \"windows-x64\"") &&
          strstr(verdict, "\"recorded_codegen\": \"6a0b6aaa\""), "same-build verdict: %s", verdict);

    /* The regression case: the Linux build of the same pin. Another exe,
     * another platform and the software renderer; the same codegen hash, disc
     * file and BIOS. */
    snprintf(product_lines, sizeof product_lines,
             "exe_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
             "codegen=6a0b6aaa\nbios_crc32=1234abcd\nrenderer=software\nplatform=linux-x64\n");
    cold_boot();
    power_on_now = 1;
    osd_last[0] = 0;
    CHECK(replay_session_play_file(path), "another platform's build of the same pin plays (%s)", osd_last);
    CHECK(strstr(osd_last, "same build on another platform (windows-x64)") && !strstr(osd_last, "different build"),
          "it is named as the same build: %s", osd_last);
    replay_session_shutdown();
    read_verdict();
    CHECK(play_from_boot(path, frames + 10u, 1) == REPLAY_RESULT_IN_SYNC, "cross-platform replay in sync (%s)", osd_last);
    CHECK(read_verdict() && strstr(verdict, "\"result\": \"in_sync\"") && strstr(verdict, "\"cross_platform\": true") &&
          strstr(verdict, "\"recorded_platform\": \"windows-x64\"") && strstr(verdict, "\"player_platform\": \"linux-x64\"") &&
          strstr(verdict, "\"player_codegen\": \"6a0b6aaa\"") && strstr(verdict, "\"player_exe_sha256\": \"bbbb"),
          "cross-platform verdict: %s", verdict);

    /* The same BIOS dump under another file name: the CRC decides. */
    snprintf(product_bios_stem, sizeof product_bios_stem, "[US] scph1001 copy");
    CHECK(play_from_boot(path, frames + 10u, 0) == REPLAY_RESULT_IN_SYNC, "another BIOS file name, same CRC: plays (%s)", osd_last);
    read_verdict();
    /* No CRC on the player's side: the name is all there is, so it refuses. */
    snprintf(product_lines, sizeof product_lines,
             "exe_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
             "codegen=6a0b6aaa\nrenderer=software\nplatform=linux-x64\n");
    cold_boot();
    power_on_now = 1;
    CHECK(!replay_session_play_file(path) && strstr(osd_last, "different BIOS"),
          "another BIOS name and no CRC: refused (%s)", osd_last);
    read_verdict();
    snprintf(product_bios_stem, sizeof product_bios_stem, "scph1001");
    CHECK(replay_session_play_file(path), "no CRC, same name (case ignored): plays (%s)", osd_last);
    replay_session_shutdown();
    read_verdict();
    /* The same name with another image: refused. */
    snprintf(product_bios_stem, sizeof product_bios_stem, "SCPH1001");
    snprintf(product_lines, sizeof product_lines,
             "exe_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
             "codegen=6a0b6aaa\nbios_crc32=99999999\nrenderer=software\nplatform=linux-x64\n");
    cold_boot();
    power_on_now = 1;
    CHECK(!replay_session_play_file(path) && strstr(osd_last, "different BIOS"),
          "same BIOS name, another CRC: refused (%s)", osd_last);
    read_verdict();

    /* Another disc image and another boot mode are refused, as before. */
    snprintf(product_lines, sizeof product_lines,
             "exe_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
             "codegen=6a0b6aaa\nbios_crc32=1234abcd\nrenderer=software\nplatform=linux-x64\n");
    product_disc_byte = 0xCD;
    CHECK(!replay_session_play_file(path) && strstr(osd_last, "different game or disc image"),
          "another disc image: refused (%s)", osd_last);
    read_verdict();
    product_disc_byte = 0xAB;
    snprintf(product_boot_mode, sizeof product_boot_mode, "lle");
    CHECK(!replay_session_play_file(path) && strstr(osd_last, "different boot mode"),
          "another boot mode: refused (%s)", osd_last);
    read_verdict();
    snprintf(product_boot_mode, sizeof product_boot_mode, "hle");

    /* Another pin is a different build on any platform. */
    product_pin[0] = product_pin[0] == 'f' ? 'e' : 'f';
    CHECK(replay_session_play_file(path) && strstr(osd_last, "different build"),
          "another pin on another platform: a different build (%s)", osd_last);
    replay_session_shutdown();
    CHECK(read_verdict() && strstr(verdict, "\"cross_platform\": false"), "another pin is not cross-platform: %s", verdict);
    snprintf(product_pin, sizeof product_pin, "%s", saved_pin);
    /* The same pin and platform with another exe is still a different build. */
    snprintf(product_lines, sizeof product_lines,
             "exe_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
             "codegen=6a0b6aaa\nbios_crc32=1234abcd\nrenderer=opengl\nplatform=windows-x64\n");
    CHECK(replay_session_play_file(path) && strstr(osd_last, "different build"),
          "same platform, another exe: a different build (%s)", osd_last);
    replay_session_shutdown();
    read_verdict();

    CHECK(strstr(replay_platform_name(), "-") != NULL && !strstr(replay_platform_name(), "unknown-unknown"),
          "this build names its platform: %s", replay_platform_name());
    snprintf(product_lines, sizeof product_lines, "%s", saved_product);
    power_on_now = 0;
    remove(path);
    free(rp);
}

static void test_boot_names(void) {
    char p[700], expect[700], partial[700], rdir[600];
    snprintf(rdir, sizeof rdir, "%s/names", dir);
    mkdir_p(rdir);
    snprintf(expect, sizeof expect, "%s/Resident_Evil_2_USA_Disc_1-boot-19700101T000000Z.psxrpl", rdir);
    remove(expect);
    CHECK(replay_session_boot_path(rdir, "Resident Evil 2 (USA) [Disc 1]", 0, p, sizeof p) && !strcmp(p, expect),
          "title reduced, UTC stamp: %s", p);
    /* 2026-10-01 12:34:56 UTC. */
    CHECK(replay_session_boot_path(rdir, "..Caf\xC3\xA9: menu/test?", INT64_C(1790858096), p, sizeof p) &&
          strstr(p, "/Caf_menu_test-boot-20261001T123456Z.psxrpl"), "no leading dots, one '_' per run: %s", p);
    CHECK(replay_session_boot_path(rdir, "", 0, p, sizeof p) && strstr(p, "/replay-boot-19700101T000000Z.psxrpl"),
          "empty title: %s", p);
    /* Taken names (or their partial copies) are never reused. */
    FILE *f = fopen(expect, "wb"); if (f) fclose(f);
    CHECK(replay_session_boot_path(rdir, "Resident Evil 2 (USA) [Disc 1]", 0, p, sizeof p) &&
          strstr(p, "Z-2.psxrpl"), "a taken name gets -2: %s", p);
    CHECK(replay_session_partial_path(p, partial, sizeof partial) && strstr(partial, "Z-2.partial.psxrpl"),
          "partial copy name: %s", partial);
    f = fopen(partial, "wb"); if (f) fclose(f);
    CHECK(replay_session_boot_path(rdir, "Resident Evil 2 (USA) [Disc 1]", 0, p, sizeof p) &&
          strstr(p, "Z-3.psxrpl"), "a crash's partial copy blocks its name too: %s", p);
    remove(expect); remove(partial);
    CHECK(replay_session_partial_path("x/y", partial, sizeof partial) && !strcmp(partial, "x/y.partial.psxrpl"),
          "partial of a name without the extension: %s", partial);
    CHECK(!replay_session_boot_path("", "t", 0, p, sizeof p), "no folder, no name");
}

int main(int argc, char **argv) {
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "replay_session_test_dir");
    mkdir_p(dir);
    snprintf(verdict_path, sizeof verdict_path, "%s/verdict.json", dir);
    replay_session_set_verdict_path(verdict_path);
    test_record_and_play_in_sync();
    test_thumb_and_name();
    test_out_of_sync_is_reported();
    test_take_over();
    test_no_free_slots();
    test_identity();
    test_shutdown_while_recording();
    test_rec_blink();
    test_power_on_record_and_play();
    test_identity_rules();
    test_boot_names();
    clear_slots();
    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: replay session, %d checks\n", checks);
    return 0;
}
