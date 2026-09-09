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
static void target(int lba) {
    int n=lba+150, m=n/4500, s=n/75%60, f=n%75;
    cdrom_write(0x1f801800,0);
    cdrom_write(0x1f801802,bin_to_bcd(m));
    cdrom_write(0x1f801802,bin_to_bcd(s));
    cdrom_write(0x1f801802,bin_to_bcd(f));
    command(2); ack();
}
static void setup(int origin) {
    psx_cycle_count=0; cdrom_init("synthetic"); reads=0;
    last_sector_lba=origin;
    int n=origin+150; read_min=n/4500; read_sec=n/75%60; read_sect=n%75;
    mode_reg=0x80; setloc_pending=0;
}

/* PSX-SPX plain seek stops future reads and finishes paused. Octoshock
 * Command_SeekL/P also preserves the already admitted DMA buffer. Do not
 * conflate that guest-owned payload with a queued, unannounced INT1. */
static void exercise_explicit(uint8_t read, uint8_t seek, int delivered) {
    setup(100); target(100); command(read); ack();
    if(delivered) { advance(read_delay); ack(); }
    target(1000);
    /* Authored pending producer state, independent of guest response FIFO. */
    pending_dataready=1; pending_dataready_stat=CDSTAT_READ|CDSTAT_MOTOR;
    pending_present_due=psx_cycle_count+500; s_cd_timing_pending_seq=123;
    RB_.size=2048;RB_.pos=4;RB_.data[4]=0x73;request_reg=CDROM_REQUEST_BFRD;
    int before_reads=reads; uint8_t before_status=stat_reg;
    int delay=seek_complete_delay_cycles();
    command(seek);
    CHECK(irq_flag==CDIRQ_ACK&&response_count==1&&response_fifo[response_read]==before_status,
          "plain seek ACK reports pre-transition drive state");
    CHECK(reading==0&&read_delay==0,"plain seek cancels pending or active read producer");
    CHECK(!pending_dataready&&pending_present_due==0,"plain seek cancels unannounced old INT1");
    CHECK((stat_reg&(CDSTAT_SEEK|CDSTAT_READ|CDSTAT_PLAY))==CDSTAT_SEEK,
          "plain seek owns only SEEK status");
    CHECK(pending.due_cyc==psx_cycle_count+(uint64_t)delay,"explicit timing is unchanged");
    CHECK(RB_.size==2048&&RB_.pos==4&&request_reg==CDROM_REQUEST_BFRD,
          "admitted guest FIFO ownership survives command");
    CHECK(cdrom_read(0x1f801802)==0x73&&RB_.pos==5,"guest can finish an admitted byte");
    ack(); advance(delay-1);
    CHECK(irq_flag==0&&reads==before_reads,"no old sector or completion before deadline");
    advance(1);
    CHECK(irq_flag==CDIRQ_COMPLETE,"plain seek emits completion at existing deadline");
    CHECK(response_fifo[response_read]==CDSTAT_MOTOR&&stat_reg==CDSTAT_MOTOR,
          "plain seek completes paused with no READ/PLAY/SEEK bit");
    CHECK(reads==before_reads&&!reading,"plain seek never resumes old stream");
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==1000,"seek retargets future stream position");
    ack();advance(2000000);
    CHECK(irq_flag==0&&reads==before_reads,"paused seek has no late INT1");
    command(read);
    CHECK(response_fifo[response_read]==CDSTAT_MOTOR,"subsequent read ACK sees paused state");
    CHECK(read_delay==initial_read_delay_cycles(),"completed seek has no extra implicit seek");
    ack();advance(read_delay);
    CHECK(last_sector_lba==1000&&reads==before_reads+1,"explicit read resumes at seek target");
    printf("read=%02x seek=%02x delivered=%d checked\n",read,seek,delivered);
}
int main(void) {
    for(int read=0;read<2;read++) for(int seek=0;seek<2;seek++)
        for(int delivered=0;delivered<2;delivered++)
            exercise_explicit(read?0x1b:6,seek?0x16:0x15,delivered);
    for(int seek=0;seek<2;seek++) {
        setup(100);target(1000);cdda_playing=1;stat_reg=CDSTAT_MOTOR|CDSTAT_PLAY;
        command(seek?0x16:0x15);
        CHECK(response_fifo[response_read]==(CDSTAT_MOTOR|CDSTAT_PLAY),"audio seek ACK keeps prior PLAY");
        CHECK(!cdda_playing&&!(stat_reg&CDSTAT_PLAY),"plain seek cancels audio producer");
    }
    if(failures){fprintf(stderr,"FAILED (%d)\n",failures);return 1;}
    puts("CD explicit seek: ALL PASS");return 0;
}
