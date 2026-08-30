/*
 * Validate the production CD-ROM seek helpers and snapshot round trip.
 *
 * Build/run: ctest -R cdrom_seek_semantics_test
 */
#include "cdrom.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t psx_cycle_count;
uint32_t i_stat;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint64_t s_frame_count;

/* cdrom.c is compiled as one production translation unit. These inert host
 * edges satisfy its linker contract; the test does not execute them. */
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void event_ring_record(uint16_t kind, uint8_t detail) { (void)kind; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind; (void)a; (void)b;
}
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
void spu_cd_audio_push(const int16_t *stereo, int frames) {
    (void)stereo; (void)frames;
}
void spu_cd_audio_reset(void) {}
int psx_netplay_active(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }

void *iso_open(const char *path) { (void)path; return NULL; }
void iso_close(void *handle) { (void)handle; }
int iso_read_sector(void *handle, uint32_t lba, uint8_t *buffer, int size) {
    (void)handle; (void)lba; (void)buffer; (void)size; return 0;
}
int iso_read_raw_sector(void *handle, uint32_t lba, uint8_t *buffer, int size) {
    (void)handle; (void)lba; (void)buffer; (void)size; return 0;
}
int iso_read_subq(void *handle, uint32_t lba, uint8_t *buffer, int size,
                  int *valid) {
    (void)handle; (void)lba; (void)buffer; (void)size;
    if (valid) *valid = 0;
    return 0;
}
int iso_has_subq_replacements(void *handle) { (void)handle; return 0; }
uint32_t iso_sector_count(void *handle) { (void)handle; return 0; }
int iso_track_count(void *handle) { (void)handle; return 0; }
uint32_t iso_track_start_lba(void *handle, int track) {
    (void)handle; (void)track; return 0;
}
uint32_t iso_track_pregap_lba(void *handle, int track) {
    (void)handle; (void)track; return 0;
}
int iso_track_is_audio(void *handle, int track) {
    (void)handle; (void)track; return 0;
}

int cdrom_test_explicit_seek_ownership(void);
int cdrom_test_implicit_seek_sequence(void);
void cdrom_test_set_setloc_state(int far_state, int pending_state);
int cdrom_test_get_setloc_state(void);

static int failures;

#define CHECK(condition, label) do {                                         \
    if (!(condition)) {                                                      \
        fprintf(stderr, "FAIL: %s\n", (label));                              \
        failures++;                                                          \
    }                                                                        \
} while (0)

int main(void) {
    uint32_t snapshot_size;
    uint8_t *snapshot;

    CHECK(cdrom_test_explicit_seek_ownership() == 0,
          "SeekL/SeekP clears the old stream, pending INT1, ring, and BFRD");
    CHECK(cdrom_test_implicit_seek_sequence() == 0,
          "Setloc plus ReadN keeps SEEK until the first sector is eligible");

    snapshot_size = cdrom_snapshot_bytes();
    snapshot = (uint8_t *)malloc(snapshot_size);
    CHECK(snapshot != NULL, "snapshot allocation");
    if (snapshot) {
        cdrom_test_set_setloc_state(1, 1);
        cdrom_snapshot_write(snapshot);
        cdrom_test_set_setloc_state(0, 0);
        CHECK(cdrom_snapshot_read(snapshot, snapshot_size) == 1,
              "snapshot parser accepts the unchanged wire size");
        CHECK(cdrom_test_get_setloc_state() == 3,
              "snapshot restores far-seek and pending-Setloc bits");
        free(snapshot);
    }

    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    puts("CD-ROM seek semantics: ALL PASS");
    return 0;
}
