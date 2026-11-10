/* PS1B-109 Dino Crisis 4395, return 4117: an XA audio sector with the
 * end-of-file bit (submode E4h) that passes the filter (Mode C8h, file 01h,
 * channel 13h). Synthetic sector only; no BIOS or game data. Includes the
 * production unit and drives its sector delivery directly.
 * Default: the data-end interrupt is pending and Setmode bit 1 stops the read.
 * PSX_CD_XA_EOF_MODEL=octoshock-2.2.2: nothing pending, the read goes on. */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
static int failures;
static uint8_t sector_file = 0x01, sector_channel = 0x13, sector_submode = 0xE4;
void psx_irq_raise(uint32_t a, uint32_t b) { (void)b; i_stat |= 1u << a; }
void event_ring_record(uint16_t a, uint8_t b) { (void)a; (void)b; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void audio_trace_event(uint16_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
void spu_cd_audio_push(const int16_t *a, int b) { (void)a; (void)b; }
void spu_cd_audio_reset(void) {}
int psx_netplay_active(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
void *iso_open(const char *p) { (void)p; return (void *)1; }
void iso_close(void *p) { (void)p; }
int iso_read_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba; memset(b, 0, (size_t)n); return 1;
}
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba;
    memset(b, 0, (size_t)n);
    b[15] = CDROM_SECTOR_MODE2;
    b[16] = b[20] = sector_file;
    b[17] = b[21] = sector_channel;
    b[18] = b[22] = sector_submode;
    b[19] = b[23] = 0x04;
    return 1;
}
int iso_read_subq(void *p, uint32_t lba, uint8_t *b, int n, int *v) {
    (void)p; (void)lba; (void)b; (void)n; *v = 0; return 0;
}
int iso_has_subq_replacements(void *p) { (void)p; return 0; }
uint32_t iso_sector_count(void *p) { (void)p; return 333000; }
int iso_track_count(void *p) { (void)p; return 1; }
uint32_t iso_track_start_lba(void *p, int t) { (void)p; (void)t; return 0; }
uint32_t iso_track_pregap_lba(void *p, int t) { (void)p; (void)t; return 0; }
int iso_track_is_audio(void *p, int t) { (void)p; (void)t; return 0; }

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)
static void set_model(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
/* One delivery of the sector at LBA 54252 (D3ECh) while reading. */
static void deliver(const char *model, uint8_t mode, uint8_t channel) {
    set_model("PSX_CD_XA_EOF_MODEL", model);
    cdrom_init("synthetic");
    psx_cycle_count = 0;
    s_nymashock_drive = 0;
    mode_reg = mode;
    filter_file = 0x01;
    filter_channel = 0x13;
    sector_channel = channel;
    xa_data_end_pending = 0;
    reading = 1;
    int n = 54252 + 150;
    read_sector_at(n / 4500, n / 75 % 60, n % 75);
}
int main(void) {
    deliver("", 0xC8, 0x13);
    CHECK(xa_data_end_pending == 1, "default: EOF sector on the filter raises data end");
    deliver("", 0xCA, 0x13);
    CHECK(xa_data_end_pending == 1 && reading == 0, "default: auto-pause stops the read");
    deliver("octoshock-2.2.2", 0xC8, 0x13);
    CHECK(xa_data_end_pending == 0, "octoshock: EOF sector on the filter raises nothing (Dino 4117)");
    CHECK(reading == 1, "octoshock: the read goes on");
    deliver("octoshock-2.2.2", 0xCA, 0x13);
    CHECK(xa_data_end_pending == 0 && reading == 1, "octoshock: auto-pause does not stop the read");
    deliver("octoshock-2.2.2", 0xC8, 0x11);
    CHECK(xa_data_end_pending == 0, "octoshock: off-filter EOF sector raises nothing");
    deliver("default", 0xC8, 0x11);
    CHECK(xa_data_end_pending == 0, "default: off-filter EOF sector raises nothing");
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}

#include "cdrom_iso_fixture_adapter.h"
