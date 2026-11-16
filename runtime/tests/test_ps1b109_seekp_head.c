/* Controller-level regression. Synthetic sectors only; no BIOS/game data.
 * Include the production unit to seed the laser position without reading an
 * entire synthetic disc. All commands and advancement use its real MMIO API.
 */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
static int reads, failures, irq_raises;
void psx_irq_raise(uint32_t a, uint32_t b) { (void)b; i_stat |= 1u << a; irq_raises++; }
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
    (void)p; memset(b, (int)(lba & 255u), (size_t)n); reads++; return 1;
}
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba; (void)b; (void)n; return 0;
}
int iso_read_subq(void *p, uint32_t lba, uint8_t *b, int n, int *v) {
    (void)p; (void)lba; (void)b; (void)n; *v=0; return 0;
}
int iso_has_subq_replacements(void *p) { (void)p; return 0; }
uint32_t iso_sector_count(void *p) { (void)p; return 333000; }
int iso_track_count(void *p) { (void)p; return 1; }
uint32_t iso_track_start_lba(void *p, int t) { (void)p; (void)t; return 0; }
uint32_t iso_track_pregap_lba(void *p, int t) { (void)p; (void)t; return 0; }
int iso_track_is_audio(void *p, int t) { (void)p; (void)t; return 0; }


#define CHECK(c, m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); failures++; } } while(0)
static void ack(void) {
    cdrom_write(0x1f801800,1); cdrom_write(0x1f801803,0x1f);
    cdrom_write(0x1f801800,0);
}
static void command(uint8_t c) { cdrom_write(0x1f801800,0); cdrom_write(0x1f801801,c); }
static void advance(int n) { psx_cycle_count += (uint32_t)n; cdrom_advance((uint32_t)n); }
static void set_model(const char *key,const char *value) {
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
}
static void target(int lba) {
    int n=lba+150;
    cdrom_write(0x1f801800,0);
    cdrom_write(0x1f801802,bin_to_bcd(n/4500));
    cdrom_write(0x1f801802,bin_to_bcd(n/75%60));
    cdrom_write(0x1f801802,bin_to_bcd(n%75));
    command(2);ack();
}
static void finish(void) { ack();advance((int)(pending.due_cyc-psx_cycle_count));ack(); }

/* Drive profile SeekP: head at target - 8 at completion, sub-Q target - 9,
 * hold 0, window target - 9 .. target - 1; a read from there to the target is
 * a near seek (FFIX 6726, original state after returns 1896 and 1897). */
static void seekp(int t) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    cdrom_init("synthetic");psx_cycle_count=0;s_nymashock_drive=1;s_source_clock=0;
    mode_reg=0x80;target(t);command(0x16);finish();
}
int main(void) {
    const int T=163047;
    seekp(T);
    CHECK(source_drive_head_valid,"SeekP completion starts the head model");
    CHECK(source_drive_head_lba==T-8,"head rests at target - 8 after SeekP");
    CHECK(source_drive_subq_lba==T-9,"sub-Q decoded at target - 9");
    CHECK(source_drive_hold_logical==0,"SeekP holds no logical position");
    uint64_t period=225792;
    psx_cycle_count=source_drive_head_due-1;source_drive_head_update();
    CHECK(source_drive_head_lba==T-8,"no advance before one period");
    int seen_low=T, seen_high=T-100;
    for (int k=0;k<18;k++) {
        psx_cycle_count=source_drive_head_due;source_drive_head_update();
        if (source_drive_head_lba<seen_low) seen_low=source_drive_head_lba;
        if (source_drive_head_lba>seen_high) seen_high=source_drive_head_lba;
    }
    CHECK(seen_low==T-9 && seen_high==T-1,"standby window is target - 9 .. target - 1");
    (void)period;

    seekp(T);
    s_source_clock=1;s_source_clock_tape.bytes=calloc(1,4);s_source_clock_tape.count=1;s_source_clock_tape.cursor=0;
    setloc_pending=1;s_setloc_lba=T;s_source_seek_paused=0;stat_reg|=CDSTAT_MOTOR;reading=0;
    int d=implicit_read_seek_cycles();
    printf("seekp head=%d read seek=%d\n",source_drive_head_lba,d);
    CHECK(d==20000+903168,"ReadS eight sectors above the head pays the near-seek term");
    printf("failures=%d\n",failures);
    return failures?1:0;
}

#include "cdrom_iso_fixture_adapter.h"
