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
static int exercise(uint8_t cmd, int origin, int dest, int observe) {
    setup(origin); target(dest); command(cmd);
    int deadline=read_delay;
    printf("cmd=%02x origin=%d target=%d ack=%02x status=%02x first_delay=%d\n",
           cmd,origin,dest,response_fifo[response_read],stat_reg,deadline);
    CHECK(response_fifo[response_read]==CDSTAT_MOTOR,"Read ACK contains pre-seek state");
    CHECK((stat_reg&(CDSTAT_SEEK|CDSTAT_READ|CDSTAT_PLAY))==CDSTAT_SEEK,
          "pending implicit seek has only SEEK state");
    if(observe) {
        ack(); advance(deadline-1); CHECK(reads==0,"no sector before combined seek/read deadline");
        advance(1); CHECK(reads==1,"one target sector at deadline");
        CHECK(last_sector_lba==dest,"first delivered sector is requested target");
        CHECK((stat_reg&(CDSTAT_SEEK|CDSTAT_READ|CDSTAT_PLAY))==CDSTAT_READ,
              "first sector changes SEEK to READ");
        ack(); advance(225791); CHECK(reads==1,"steady 2x cadence unchanged before deadline");
        advance(1); CHECK(reads==2,"steady 2x cadence delivers next sector");
        CHECK(last_sector_lba==dest+1,"stream advances consecutively");
    }
    return deadline;
}
int main(void) {
#ifdef _WIN32
#define SET_MODEL(v) _putenv_s("PSX_CD_READ_START_MODEL", (v))
#else
#define SET_MODEL(v) setenv("PSX_CD_READ_START_MODEL", (v), 1)
#endif
    SET_MODEL("");
    int near_delay=exercise(0x06,103948,103949,1);
    int far_delay=exercise(0x06,103948,251926,1);
    int reverse=exercise(0x1b,251926,103948,1);
    CHECK(far_delay>near_delay+10000000,"far implicit seek includes distance latency");
    CHECK(reverse==far_delay,"ReadS uses the same distance rule in reverse direction");
    /* Same target while already reading must preserve the buffered stream. */
    setup(100); reading=1; stat_reg=CDSTAT_MOTOR|CDSTAT_READ;
    read_delay=12345; RB_.size=2048; RB_.pos=4;
    target(100); command(6);
    CHECK(read_delay==12345&&RB_.size==2048&&RB_.pos==4,"same-stream read is not restarted");
    /* Existing command latency is independent of the long sector deadline. */
    setup(103948); target(251926); command(6);
    CHECK(cdrom_irq_present_due==CDROM_IRQ_PRESENT_DELAY,"ACK presentation latency unchanged");
    ack(); command(9); ack(); advance(100000000);
    CHECK(reads==0,"Pause cancels an implicit seek before any sector is delivered");
    /* A data-ready result is asynchronous, unlike a response created inside
     * the guest's command store. Its readable flag and INTC line must agree.
     * Otherwise the SDK can clear a newly visible INT1 before INTC ever sees
     * it when preparing its next GetStat, losing the first sector header. */
    setup(100); target(200); command(6); ack(); i_stat=0; irq_raises=0;
    advance(read_delay);
    CHECK(irq_flag==CDIRQ_DATA_READY,"first target owns the visible response");
    CHECK((i_stat&(1u<<2))!=0,"async INT1 reaches INTC when the ready flag becomes visible");
    CdTimingPub timing;
    CHECK(cdrom_timing_record(cdrom_timing_total()-1,&timing)&&
          timing.lba==200&&timing.intc_cycle==psx_cycle_count&&
          timing.irq_arm_cycle==timing.intc_cycle,
          "immediate INTC timestamp belongs to the sector that raised it");
    command(1);
    CHECK(queued_cmd.pending&&irq_flag==CDIRQ_DATA_READY,
          "GetStat cannot replace an outstanding sector response");
    CHECK(last_sector_lba==200&&s_ring_read==s_ring_write,
          "queued GetStat preserves the first target ring owner");
    advance(2472);
    CHECK(irq_raises==1,"unacked asynchronous INT1 raises INTC once");
    ack(); i_stat=0; irq_raises=0;
    CHECK(irq_flag==CDIRQ_ACK,"acknowledging data-ready releases queued GetStat");
    advance(CDROM_IRQ_PRESENT_DELAY-1);
    CHECK(i_stat==0,"queued command retains command-response delay");
    advance(1);
    CHECK((i_stat&(1u<<2))&&irq_raises==1,"queued command ACK presents once at its own deadline");
    /* An interrupt mask suppresses INTC, without hiding the controller flag. */
    setup(100); target(200); command(6); ack(); irq_enable=0; i_stat=0;
    advance(read_delay);
    CHECK(irq_flag==CDIRQ_DATA_READY&&i_stat==0,"masked async result retains its pending flag");
    irq_enable=7; advance(0);
    CHECK((i_stat&(1u<<2))!=0,"enabling a pending async result presents INTC");
    /* Source-profile deadline control: a two-slot source pipeline does not
     * present the first sector until its third fetch. Default stays two. */
    for (int double_speed=0; double_speed<=1; ++double_speed) {
        SET_MODEL(""); setup(100); mode_reg=double_speed?0x80:0;
        target(200); command(6); int baseline=read_delay;
        SET_MODEL("octoshock-2.2.2-pipeline");
        setup(100); mode_reg=double_speed?0x80:0; target(200); command(6);
        int period=double_speed?225792:451584;
        CHECK(read_delay==baseline+period,"profile adds exactly one sector period to first delivery");
        ack(); advance(baseline); CHECK(reads==0,"profile target not presented at old deadline");
        advance(period-1); CHECK(reads==0,"profile target not presented before third-fetch deadline");
        advance(1); CHECK(reads==1&&last_sector_lba==200,"profile presents requested target at third-fetch deadline");
        ack(); advance(period); CHECK(reads==2&&last_sector_lba==201,"profile preserves subsequent sector cadence");
    }
    SET_MODEL("");
    if(failures) { fprintf(stderr,"FAILED (%d)\n",failures); return 1; }
    puts("CD implicit seek: ALL PASS"); return 0;
}
