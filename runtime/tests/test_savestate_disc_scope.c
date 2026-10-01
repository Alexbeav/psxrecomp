/* test_savestate_disc_scope.c — save states of a game on several discs.
 *
 * A save state restores the drive's registers and buffers, not the image in
 * the drive. Its file name carries the disc it was taken on. These tests run
 * the real savestate.c against a stub serializer and a stub frontend:
 *
 *   - a disc change renames what is saved next, and clears rewind;
 *   - the sequence a player ran on Final Fantasy VII: launch on disc 1, change
 *     to disc 2, save, relaunch on disc 1, load. The state is named for disc
 *     2, and the load mounts disc 2 first;
 *   - the load is refused, with a sentence naming the disc, when that disc
 *     cannot be mounted, and the running game keeps its disc;
 *   - a refused load after a mount puts the previous disc back.
 *
 *   test_savestate_disc_scope DIR   (DIR must exist; the test works in a
 *                                    subfolder and removes its own files)
 */
#include "savestate.h"
#include "boot_state.h"
#include "cdrom.h"
#include "gpu.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_netplay_rb.h"
#include "psx_rewind.h"
#include "psx_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#if defined(_WIN32)
#include <sys/utime.h>
#define set_file_time _utime
typedef struct _utimbuf file_times;
#else
#include <utime.h>
#define set_file_time utime
typedef struct utimbuf file_times;
#endif

static int s_fails = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); s_fails++; } } while (0)

#define BIOS_SUM 0x1234ABCDu
#define ENTRY_PC 0x80010008u
#define RESUME_PC 0x80012340u
/* Modified times for planted files: three days in 2023, far apart. */
#define T_OLD 1700000000L
#define T_MID 1701000000L
#define T_NEW 1702000000L

/* ---- stub serializer (boot_state.c) -------------------------------------- */

static char s_saved_path[700];
static char s_loaded_path[700];
static int  s_load_calls;
static int  s_load_result = 1;
static int  s_header_ok = 1;

int boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                    uint32_t entry_pc, const char* path) {
    FILE* f;
    (void)cpu; (void)bios_checksum; (void)entry_pc;
    snprintf(s_saved_path, sizeof(s_saved_path), "%s", path);
    f = fopen(path, "wb");
    if (!f) return 0;
    fputs("stub state", f);
    return fclose(f) == 0;
}
int boot_state_load(const char* path, uint32_t bios_checksum,
                    uint32_t entry_pc, CPUState* cpu) {
    (void)bios_checksum; (void)entry_pc;
    s_load_calls++;
    snprintf(s_loaded_path, sizeof(s_loaded_path), "%s", path);
    if (s_load_result) cpu->pc = RESUME_PC;
    return s_load_result;
}
int boot_state_peek_cpu_pc(const char* path, uint32_t* out_pc) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    *out_pc = RESUME_PC;
    return 1;
}
int boot_state_check_buffer(const uint8_t* file, size_t file_len,
                            uint32_t bios_checksum, uint32_t entry_pc,
                            char* reason, size_t reason_cap) {
    (void)file; (void)file_len; (void)bios_checksum; (void)entry_pc;
    if (reason && reason_cap) snprintf(reason, reason_cap, "%s", s_header_ok ? "" : "stub");
    return s_header_ok;
}
int boot_state_save_buffer(const CPUState* cpu, uint32_t bios_checksum,
                           uint32_t entry_pc, uint8_t** out_data, size_t* out_len) {
    (void)cpu; (void)bios_checksum; (void)entry_pc; (void)out_data; (void)out_len;
    return 0;
}
int boot_state_load_buffer(const uint8_t* file, size_t file_len,
                           uint32_t bios_checksum, uint32_t entry_pc, CPUState* cpu) {
    (void)file; (void)file_len; (void)bios_checksum; (void)entry_pc; (void)cpu;
    return 0;
}
int boot_state_peek_cpu_pc_buffer(const uint8_t* file, size_t file_len, uint32_t* out_pc) {
    (void)file; (void)file_len; (void)out_pc;
    return 0;
}

/* ---- stub machine --------------------------------------------------------- */

static int s_netplay;
static int s_resumed;
static int s_rewind_clears;

void cdrom_accelerate_after_savestate(void) {}
void gpu_get_display_info(GpuDisplayInfo* out) { memset(out, 0, sizeof(*out)); out->disabled = 1; }
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* di, uint32_t x, uint32_t y) {
    (void)di; (void)x; (void)y;
    return 0;
}
void interrupts_resync_after_restore(void) {}
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t resume_pc) { (void)resume_pc; return 1; }
int psx_irq_resume_context_snapshot_site(void) { return 0; }
uint32_t psx_irq_resume_context_snapshot_pc(void) { return 0; }
void psx_cycles_resync_after_restore(struct CPUState* cpu) { (void)cpu; }
int psx_netplay_active(void) { return s_netplay; }
int psx_netplay_is_host(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int psx_is_dispatchable(uint32_t pc) { return pc != 0; }
int psx_hle_scheduler_enabled(void) { return 1; }
/* The real one unwinds to the scheduler; here the poll simply returns. */
void psx_scheduler_resume_at(uint32_t resume_pc) { (void)resume_pc; s_resumed++; }
void psx_rewind_clear(void) { s_rewind_clears++; }

/* ---- stub frontend (main.cpp) --------------------------------------------- */

static int  s_mount_calls, s_mount_disc, s_mount_allow = 1;
static int  s_result_calls, s_result_disc, s_result_kept;
static int  s_notify_calls, s_notify_load, s_notify_ok;
static int  s_refused_calls;
static char s_refused_text[400];

void psx_frontend_on_savestate_loaded(void) {}
void psx_frontend_on_savestate_notify(int is_load, int slot, int ok) {
    (void)slot;
    s_notify_calls++;
    s_notify_load = is_load;
    s_notify_ok = ok;
}
int psx_frontend_savestate_mount_disc(int disc_number, char* why, size_t why_cap) {
    s_mount_calls++;
    s_mount_disc = disc_number;
    if (!s_mount_allow) snprintf(why, why_cap, "its image (SCUS-94164) was not found");
    return s_mount_allow;
}
void psx_frontend_savestate_mount_result(int disc_number, int kept) {
    s_result_calls++;
    s_result_disc = disc_number;
    s_result_kept = kept;
}
void psx_frontend_on_savestate_refused(int slot, const char* text) {
    (void)slot;
    s_refused_calls++;
    snprintf(s_refused_text, sizeof(s_refused_text), "%s", text);
}

/* ---- helpers --------------------------------------------------------------- */

static char s_root[500];
static CPUState s_cpu;
static const int k_set[3] = { 1, 2, 3 };

static void reset_counters(void) {
    s_saved_path[0] = s_loaded_path[0] = s_refused_text[0] = '\0';
    s_load_calls = s_resumed = s_rewind_clears = 0;
    s_mount_calls = s_mount_disc = 0;
    s_result_calls = s_result_disc = 0;
    s_result_kept = -1;
    s_notify_calls = s_refused_calls = 0;
    s_notify_load = s_notify_ok = -1;
    s_load_result = 1;
    s_header_ok = 1;
    s_mount_allow = 1;
    s_netplay = 0;
}

static void state_name(int disc, int slot, const char* ext, char* out, size_t cap) {
    char token[16];
    token[0] = '\0';
    if (disc) snprintf(token, sizeof(token), "_disc%d", disc);
    snprintf(out, cap, "%s/scph1001/state_%08X%s_slot%02d.%s", s_root,
             (unsigned)ENTRY_PC, token, slot, ext);
}

static int state_exists(int disc, int slot) {
    char path[700];
    FILE* f;
    state_name(disc, slot, "pst", path, sizeof(path));
    f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* A state file as an earlier session left it, with a chosen modified time. */
static void plant_state(int disc, int slot, long when) {
    char path[700];
    file_times times;
    FILE* f;
    state_name(disc, slot, "pst", path, sizeof(path));
    f = fopen(path, "wb");
    if (!f) { CHECK(!"cannot plant a state file"); return; }
    fputs("planted state", f);
    fclose(f);
    times.actime = (time_t)when;
    times.modtime = (time_t)when;
    CHECK(set_file_time(path, &times) == 0);
}

static void remove_all_states(void) {
    char path[700];
    int disc, slot;
    for (disc = 0; disc <= 3; disc++) {
        for (slot = 0; slot < SAVESTATE_SLOTS; slot++) {
            state_name(disc, slot, "pst", path, sizeof(path));
            remove(path);
            state_name(disc, slot, "thumb", path, sizeof(path));
            remove(path);
        }
    }
}

/* A launch: the scope and the roster are set before configure, as main does. */
static void launch(int mounted_disc, const int* roster, int roster_count) {
    savestate_set_disc_scope(mounted_disc);
    savestate_set_disc_roster(roster, roster_count);
    savestate_configure(s_root, BIOS_SUM, ENTRY_PC, "scph1001", 0);
    reset_counters();
    memset(&s_cpu, 0, sizeof(s_cpu));
    s_cpu.pc = RESUME_PC;
}

static void save_slot(int slot) {
    CHECK(savestate_request_save(slot) == 1);
    savestate_poll(&s_cpu, RESUME_PC);
    CHECK(savestate_pending() == 0);
}

static void load_slot(int slot) {
    CHECK(savestate_request_load(slot) == 1);
    savestate_poll(&s_cpu, RESUME_PC);
    CHECK(savestate_pending() == 0);
}

static int ends_with(const char* text, const char* tail) {
    const size_t n = strlen(text), m = strlen(tail);
    return n >= m && strcmp(text + n - m, tail) == 0;
}

/* ---- the rules -------------------------------------------------------------- */

/* A disc change renames what is saved next, and drops rewind. */
static void test_disc_change_renames_and_clears_rewind(void) {
    char path[700];
    remove_all_states();
    launch(1, k_set, 3);
    CHECK(savestate_disc_scope() == 1);
    save_slot(0);
    CHECK(ends_with(s_saved_path, "_disc1_slot00.pst"));

    savestate_note_disc_mounted(2);             /* the player's "Change disc" */
    CHECK(savestate_disc_scope() == 2);
    CHECK(s_rewind_clears == 1);
    save_slot(1);
    CHECK(ends_with(s_saved_path, "_disc2_slot01.pst"));
    CHECK(state_exists(2, 1) && !state_exists(1, 1));
    /* Replays sit beside the states and take their name from this path. */
    CHECK(savestate_slot_path(0, path, sizeof(path)) && ends_with(path, "_disc2_slot00.pst"));
}

/* A title on one disc keeps its names; a disc change still drops rewind. */
static void test_single_disc_title_keeps_its_names(void) {
    remove_all_states();
    launch(0, NULL, 0);
    savestate_note_disc_mounted(0);
    CHECK(savestate_disc_scope() == 0);
    CHECK(s_rewind_clears == 1);
    savestate_note_disc_mounted(2);             /* no set: the number means nothing */
    CHECK(savestate_disc_scope() == 0);
    save_slot(3);
    CHECK(ends_with(s_saved_path, "state_80010008_slot03.pst"));
    CHECK(savestate_slot_disc(3) == 0);
    load_slot(3);
    CHECK(s_load_calls == 1 && s_mount_calls == 0 && s_resumed == 1);
}

/* Launch on disc 1, change to disc 2, save, relaunch on disc 1, load. */
static void test_player_sequence_mounts_the_states_disc(void) {
    remove_all_states();
    launch(1, k_set, 3);
    savestate_note_disc_mounted(2);
    save_slot(0);
    CHECK(state_exists(2, 0));
    CHECK(!state_exists(1, 0));                 /* pin G2 wrote this name */

    launch(1, k_set, 3);                        /* the relaunch: disc 1 again */
    CHECK(savestate_disc_scope() == 1);
    CHECK(savestate_slot_exists(0) == 1);
    CHECK(savestate_slot_disc(0) == 2);
    load_slot(0);
    CHECK(s_mount_calls == 1 && s_mount_disc == 2);
    CHECK(s_load_calls == 1 && ends_with(s_loaded_path, "_disc2_slot00.pst"));
    CHECK(s_result_calls == 1 && s_result_disc == 2 && s_result_kept == 1);
    CHECK(savestate_disc_scope() == 2);
    CHECK(s_rewind_clears == 1);
    CHECK(s_notify_calls == 1 && s_notify_load == 1 && s_notify_ok == 1);
    CHECK(s_refused_calls == 0);
    CHECK(s_resumed == 1);
    CHECK(savestate_take_load_completed() == 1);
    save_slot(4);                               /* disc 2 is in the drive now */
    CHECK(ends_with(s_saved_path, "_disc2_slot04.pst"));
}

/* The state's disc cannot be mounted: refuse, name the disc, change nothing. */
static void test_load_refused_when_the_disc_is_absent(void) {
    remove_all_states();
    launch(1, k_set, 3);
    plant_state(2, 0, T_OLD);
    s_mount_allow = 0;
    load_slot(0);
    CHECK(s_mount_calls == 1 && s_mount_disc == 2);
    CHECK(s_load_calls == 0);
    CHECK(s_result_calls == 0);
    CHECK(s_refused_calls == 1);
    CHECK(strstr(s_refused_text, "Slot 1 needs disc 2") != NULL);
    CHECK(strstr(s_refused_text, "SCUS-94164") != NULL);
    CHECK(s_notify_calls == 0);
    CHECK(savestate_disc_scope() == 1);
    CHECK(s_rewind_clears == 0);
    CHECK(s_resumed == 0);
    CHECK(savestate_take_load_failed() == 1);
}

/* The disc mounted but the load was refused: the previous disc goes back. */
static void test_refused_load_puts_the_disc_back(void) {
    remove_all_states();
    launch(1, k_set, 3);
    plant_state(2, 0, T_OLD);
    s_load_result = 0;
    load_slot(0);
    CHECK(s_mount_calls == 1 && s_load_calls == 1);
    CHECK(s_result_calls == 1 && s_result_disc == 2 && s_result_kept == 0);
    CHECK(savestate_disc_scope() == 1);
    CHECK(s_rewind_clears == 0);
    CHECK(s_notify_calls == 1 && s_notify_load == 1 && s_notify_ok == 0);
    CHECK(s_resumed == 0);
}

/* A state this build cannot load is refused before any disc is mounted. */
static void test_incompatible_state_mounts_nothing(void) {
    remove_all_states();
    launch(1, k_set, 3);
    plant_state(2, 0, T_OLD);
    s_header_ok = 0;
    load_slot(0);
    CHECK(s_mount_calls == 0 && s_load_calls == 0 && s_result_calls == 0);
    CHECK(s_notify_calls == 1 && s_notify_ok == 0);
    CHECK(savestate_disc_scope() == 1);
}

/* Which state a slot shows. */
static void test_slot_view(void) {
    remove_all_states();
    launch(1, k_set, 3);
    CHECK(savestate_slot_exists(0) == 0 && savestate_slot_disc(0) == 0);

    /* The mounted disc's own state comes first, whatever its age. */
    plant_state(1, 0, T_OLD);
    plant_state(2, 0, T_MID);
    CHECK(savestate_slot_disc(0) == 1);
    load_slot(0);
    CHECK(s_mount_calls == 0 && s_load_calls == 1);
    CHECK(ends_with(s_loaded_path, "_disc1_slot00.pst"));

    /* Otherwise the newest state another disc left in the slot. */
    plant_state(2, 5, T_MID);
    plant_state(3, 5, T_NEW);
    CHECK(savestate_slot_disc(5) == 3);
    plant_state(2, 6, T_NEW);
    plant_state(3, 6, T_MID);
    CHECK(savestate_slot_disc(6) == 2);
    {
        /* The slot reports the shown state's time. Two hours of slack for a
         * C runtime that applies daylight saving to file times. */
        int64_t when = 0;
        CHECK(savestate_slot_mtime(5, &when) == 1);
        CHECK(when > T_NEW - 7200 && when < T_NEW + 7200);
    }

    /* Saving writes the mounted disc's file and leaves the other disc's. */
    save_slot(5);
    CHECK(ends_with(s_saved_path, "_disc1_slot05.pst"));
    CHECK(state_exists(3, 5) && savestate_slot_disc(5) == 1);

    /* A disc outside the roster is never offered. */
    launch(1, k_set, 2);
    CHECK(savestate_slot_disc(6) == 2);
    remove_all_states();
    plant_state(3, 7, T_OLD);
    CHECK(savestate_slot_exists(7) == 0 && savestate_slot_disc(7) == 0);

    /* Without a roster a slot shows only the mounted disc's state. */
    launch(1, NULL, 0);
    plant_state(2, 8, T_OLD);
    CHECK(savestate_slot_exists(8) == 0);

    /* Netplay peers exchange the mounted disc's file only. */
    launch(1, k_set, 3);
    CHECK(savestate_slot_disc(8) == 2);
    s_netplay = 1;
    CHECK(savestate_slot_disc(8) == 0 && savestate_slot_exists(8) == 0);
    s_netplay = 0;
}

int main(int argc, char** argv) {
    char probe[600];
    FILE* f;
    if (argc < 2) {
        fprintf(stderr, "usage: test_savestate_disc_scope DIR\n");
        return 2;
    }
    snprintf(s_root, sizeof(s_root), "%s/savestate_disc_scope_work", argv[1]);
    /* savestate_configure creates the folders; prove the place is writable. */
    launch(1, k_set, 3);
    snprintf(probe, sizeof(probe), "%s/scph1001/probe.tmp", s_root);
    f = fopen(probe, "wb");
    if (!f) {
        fprintf(stderr, "cannot write under %s\n", s_root);
        return 2;
    }
    fclose(f);
    remove(probe);

    test_disc_change_renames_and_clears_rewind();
    test_single_disc_title_keeps_its_names();
    test_player_sequence_mounts_the_states_disc();
    test_load_refused_when_the_disc_is_absent();
    test_refused_load_puts_the_disc_back();
    test_incompatible_state_mounts_nothing();
    test_slot_view();
    remove_all_states();

    if (s_fails) {
        fprintf(stderr, "test_savestate_disc_scope: %d check(s) failed\n", s_fails);
        return 1;
    }
    printf("test_savestate_disc_scope: 7 cases passed\n");
    return 0;
}
