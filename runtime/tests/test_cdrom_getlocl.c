/* GetlocL response type (PS1B-212). Synthetic sectors only; no BIOS/game data.
 * Includes the production unit and drives it through its real MMIO API.
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
static void target(int lba) {
    int n=lba+150, m=n/4500, s=n/75%60, f=n%75;
    cdrom_write(0x1f801800,0);
    cdrom_write(0x1f801802,bin_to_bcd(m));
    cdrom_write(0x1f801802,bin_to_bcd(s));
    cdrom_write(0x1f801802,bin_to_bcd(f));
    command(2); ack();
}
static void setup(void) {
    psx_cycle_count=0; cdrom_init("synthetic"); reads=0;
    mode_reg=0x80; setloc_pending=0;
}
/* GetlocL and return its IRQ type; the response FIFO holds its bytes. */
static int getlocl(void) { command(0x10); return irq_flag; }
static int error80(void) {
    return response_count==2 && (response_fifo[response_read]&CDSTAT_ERROR) &&
           response_fifo[(response_read+1)&15]==0x80;
}
static void wait_complete(void) {
    for(int i=0;i<4000 && irq_flag!=CDIRQ_COMPLETE;i++) advance(1000);
}
/* A read that has delivered its first data sector; the INT1 is acknowledged. */
static void start_reading(int lba) {
    setup(); target(lba); command(6); ack();
    for(int i=0;i<4000 && irq_flag!=CDIRQ_DATA_READY;i++) advance(1000);
    ack();
}

int main(void) {
    /* Power-on: no header decoded yet. */
    setup();
    CHECK(getlocl()==CDIRQ_ERROR && error80(),"GetlocL at power-on is INT5 80h");
    ack();

    /* Standby after SeekL: the seek completed, no data sector since [C6]. */
    setup(); target(1000); command(0x15); ack(); wait_complete(); ack();
    CHECK(!(stat_reg&CDSTAT_SEEK),"SeekL completed");
    CHECK(getlocl()==CDIRQ_ERROR && error80(),"standby after SeekL is INT5 80h");
    ack();

    /* ReadN issued, first data sector not yet delivered: seek phase. */
    setup(); target(200); command(6); ack();
    CHECK(getlocl()==CDIRQ_ERROR && error80(),"ReadN before its first sector is INT5 80h");
    ack();

    /* Reading: INT3 with the newest header (mode byte present). */
    start_reading(300);
    CHECK(last_sector_lba>=300,"a data sector was delivered");
    CHECK(getlocl()==CDIRQ_ACK && response_count==8,"GetlocL while reading is INT3 with 8 bytes");
    ack();

    /* Paused after reading keeps the header. */
    start_reading(400);
    command(0x09); ack(); wait_complete(); ack();
    CHECK(!reading,"Pause completed");
    CHECK(getlocl()==CDIRQ_ACK && response_count==8,"paused after reading is INT3 with the header");
    ack();

    /* An audio sector has no header: CD-DA playing is INT5 80h. */
    start_reading(500); command(0x09); ack(); wait_complete(); ack();
    cdda_playing=1;
    CHECK(getlocl()==CDIRQ_ERROR && error80(),"CD-DA track is INT5 80h");
    ack();

    if(failures){fprintf(stderr,"FAILED (%d)\n",failures);return 1;}
    puts("CD GetlocL: ALL PASS");return 0;
}
