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
static int run_seek(int fast,int logical,int paused) {
    cdrom_init("synthetic");psx_cycle_count=0;mode_reg=fast?0x80:0;target(4);
    if(!paused) { command(0x16);finish();target(5); }
    command(logical?0x15:0x16);
    int n=(int)pending.due_cyc-(int)psx_cycle_count;
    int expected=20000+(paused?(fast?1237952:2475904):0)+(logical?(fast?225792:451584):0);
    CHECK(n==expected,"source paused/standby and SeekL/P deterministic deadline");
    CHECK(response_fifo[response_read]==CDSTAT_MOTOR,"seek ACK remains pre-transition motor status");
    ack();advance(n-1);CHECK(irq_flag==0,"no early explicit completion");
    advance(1);CHECK(irq_flag==CDIRQ_COMPLETE&&stat_reg==CDSTAT_MOTOR,"exact completion paused public status");
    CHECK(s_source_seek_paused==0,"plain seek ends in source STANDBY timing state");
    return n;
}
int main(void) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","");cdrom_init("synthetic");
    unsigned default_size=cdrom_snapshot_bytes();
    uint8_t *default_a=malloc(default_size),*default_b=malloc(default_size);
    cdrom_snapshot_write(default_a);s_source_seek_paused=0;cdrom_snapshot_write(default_b);
    CHECK(!memcmp(default_a,default_b,default_size),"default wire excludes inert source-only state");
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    for(int fast=0;fast<2;fast++) for(int logical=0;logical<2;logical++) for(int paused=0;paused<2;paused++)
        printf("fast=%d logical=%d paused=%d delay=%d\n",fast,logical,paused,run_seek(fast,logical,paused));
    cdrom_init("synthetic");psx_cycle_count=0;target(4);
    unsigned size=cdrom_snapshot_bytes();CHECK(size==default_size+1,"profile adds one declared timing byte");
    uint8_t *wire=malloc(size);cdrom_snapshot_write(wire);
    command(0x15);finish();CHECK(!s_source_seek_paused,"seek changes timing state before restore");
    CHECK(cdrom_snapshot_read(wire,size)&&s_source_seek_paused==1,"matching-profile snapshot restores paused timing");
    CHECK(source_explicit_seek_cycles(0x15)==2947488,"restored first logical seek retains exact delay");
    wire[size-1]=2;uint8_t old_stat=stat_reg;uint8_t old_phase=s_source_seek_paused;
    CHECK(!cdrom_snapshot_read(wire,size)&&stat_reg==old_stat&&s_source_seek_paused==old_phase,"invalid phase fails before state mutation");
    command(0x15);finish();command(9);finish();target(5);command(0x15);
    CHECK(pending.due_cyc-psx_cycle_count==2947488,"Pause restores paused restart cost after standby");
    finish();command(8);finish();target(4);command(0x16);
    CHECK(pending.due_cyc-psx_cycle_count==33888800,"stopped motor uses source spin-up lower bound");
    cdrom_init("synthetic");target(2250);command(0x16);
    CHECK(pending.due_cyc-psx_cycle_count==10395840,"long travel uses settle term instead of paused term");
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","");cdrom_init("synthetic");target(4);command(0x15);
    CHECK(pending.due_cyc-psx_cycle_count==1806336,"default far explicit timing is unchanged");
    free(wire);free(default_a);free(default_b);
    if(failures){fprintf(stderr,"FAILED (%d)\n",failures);return 1;}
    puts("CD source explicit timing: ALL PASS");return 0;
}
