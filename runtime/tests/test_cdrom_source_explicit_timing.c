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
    /* Opt-in 1.29 drive corrections do not alter the older model above. */
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    cdrom_init("synthetic");psx_cycle_count=0;s_nymashock_drive=1;
    target(4);command(0x15);
    CHECK(pending.due_cyc==3399072,"Nymashock logical seek fills both header pipeline slots");
    finish();
    CHECK(source_drive_head_valid && source_drive_head_lba==-2,"standby retreats nine after target plus three");
    uint64_t next=source_drive_head_due;
    psx_cycle_count=next-1;source_drive_head_update();
    CHECK(source_drive_head_lba==-2,"physical head does not advance early");
    psx_cycle_count=next;source_drive_head_update();
    CHECK(source_drive_head_lba==-1,"physical head advances on sector deadline");
    /* Full source-drive checkpoint support is a separate submission. */
    s_source_clock=1;s_source_clock_tape.bytes=malloc(4);s_source_clock_tape.count=1;
    memset(s_source_clock_tape.bytes,0,4);s_source_clock_tape.bytes[0]=123;s_source_clock_tape.cursor=0;
    source_drive_head_valid=1;source_drive_head_lba=-2;source_drive_head_target=4;
    source_drive_head_due=psx_cycle_count+451584;mode_reg=0x80;s_source_seek_paused=0;
    setloc_pending=0;lba_to_msf(4,150,&read_min,&read_sec,&read_sect);
    CHECK(implicit_read_seek_cycles()==923291 && s_source_clock_tape.cursor==1,"standby short seek includes four sector intervals and one bounded random call");
    reading=1;s_source_read_start_lba=4;s_source_clock_tape.cursor=0;
    lba_to_msf(5,150,&read_min,&read_sec,&read_sect);
    CHECK(pause_complete_delay_cycles()==1124744 && s_source_clock_tape.cursor==1,"active Pause consumes its own bounded random word");
    /* Reset has independent travel, header-pipeline, and reply deadlines. */
    free(s_source_clock_tape.bytes);s_source_clock_tape.bytes=calloc(4,4);
    s_source_clock_tape.count=4;s_source_clock_tape.cursor=0;
    reading=0;stat_reg=CDSTAT_MOTOR;s_source_seek_paused=1;
    source_drive_head_valid=1;source_drive_head_lba=9;source_drive_hold_logical=1;
    psx_cycle_count=100;source_drive_head_due=UINT64_MAX;irq_flag=0;
    exec_command(0x0A);
    uint64_t travel=s_source_reset_due;
    CHECK(travel==100+2495904 && pending.due_cyc==100+4100000,"Reset separates source seek travel from reply deadline");
    CHECK(mode_reg==0x20 && source_reset_phase==1 && s_source_clock_tape.cursor==2,"Reset changes mode immediately and consumes two random draws");
    psx_cycle_count+=1000;irq_flag=0;exec_command(0x0A);
    CHECK(s_source_reset_due==travel && pending.due_cyc==psx_cycle_count+256 && s_source_clock_tape.cursor==2,"repeat Reset during travel polls without restarting or drawing random numbers");
    psx_cycle_count=travel-1;process_source_reset();
    CHECK(source_reset_phase==1,"Reset travel cannot finish early");
    psx_cycle_count=travel;process_source_reset();
    CHECK(source_reset_phase==2 && source_drive_head_lba==1 && (stat_reg&CDSTAT_SEEK),"arrival fills first logical header slot and remains seeking");
    irq_flag=0;exec_command(0x0A);
    CHECK(s_source_reset_due==travel+20000 && source_reset_phase==1 && s_source_clock_tape.cursor==4,"Reset during header verification restarts physical seek");
    psx_cycle_count=s_source_reset_due+2*451584;process_source_reset();
    CHECK(!s_source_reset_due && !source_reset_phase && source_drive_head_lba==-6 && s_source_seek_paused && !(stat_reg&CDSTAT_SEEK),"two header intervals finish Reset into paused physical position");
    irq_flag=0;s_source_ready_due=0;
    psx_cycle_count=pending.due_cyc-1;process_pending(0);
    CHECK(irq_flag==0,"finished drive does not present Reset reply before command deadline");
    psx_cycle_count++;process_pending(0);
    CHECK(irq_flag==CDIRQ_COMPLETE && !pending.pending,"Reset completion presents at independent command deadline");
    s_source_clock_tape.cursor=0;reading=1;stat_reg=CDSTAT_MOTOR|CDSTAT_READ;
    s_source_seek_paused=0;source_drive_head_lba=9;irq_flag=0;
    lba_to_msf(10,150,&read_min,&read_sec,&read_sect);
    exec_command(0x0A);
    CHECK(s_source_reset_due==psx_cycle_count+20000,"active Reset seeks from physical sector twelve, beyond the short-distance penalty");
    CHECK(!reading && !(stat_reg&CDSTAT_READ) && (stat_reg&CDSTAT_SEEK),"active Reset stops data delivery and enters seek");
    CHECK(s_source_clock_tape.cursor==2,"active Reset consumes the same two random draws");
    cdrom_init("synthetic");s_nymashock_drive=1;
    reading=1;stat_reg=CDSTAT_MOTOR|CDSTAT_READ;
    lba_to_msf(1234,150,&read_min,&read_sec,&read_sect);
    response_clear();irq_flag=0;exec_command(0x11);
    const uint8_t physical_q[8]={1,1,0,0x16,0x35,0,0x18,0x35};
    CHECK(response_count==8 && !memcmp(response_fifo,physical_q,8),"Nymashock GetlocP reports the last physical read before the data pipeline");
    s_nymashock_drive=0;response_clear();irq_flag=0;exec_command(0x11);
    CHECK(response_fifo[4]==0x34 && response_fifo[7]==0x34,"older GetlocP position remains unchanged");
    cd_tape_free(&s_source_clock_tape);s_source_clock=0;s_nymashock_drive=0;

    free(wire);free(default_a);free(default_b);
    if(failures){fprintf(stderr,"FAILED (%d)\n",failures);return 1;}
    puts("CD source explicit timing: ALL PASS");return 0;
}
