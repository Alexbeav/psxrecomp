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
static void test_version(int source) {
    static const uint8_t expected[2][4]={{0x94,0x09,0x19,0xc0},{0x97,0x01,0x10,0xc2}};
    set_model("PSX_CD_FIRMWARE_MODEL",source?"octoshock-2.2.2":"");
    cdrom_init("synthetic");
    cdrom_write(0x1f801800,0);cdrom_write(0x1f801802,0x20);command(0x19);
    CHECK(irq_flag==CDIRQ_ACK&&response_count==4,"version owns one four-byte ACK");
    for(int i=0;i<4;i++) CHECK(cdrom_read(0x1f801801)==expected[source][i],"documented controller version byte");
    ack();
}
/* Test 04h resets the SCEx counters and answers the status; Test 05h answers
 * the two counters and no status byte (PS1G-116). */
static void test_scex_counters(int source) {
    set_model("PSX_CD_FIRMWARE_MODEL",source?"octoshock-2.2.2":"");
    cdrom_init("synthetic");
    cdrom_write(0x1f801800,0);cdrom_write(0x1f801802,0x04);command(0x19);
    CHECK(irq_flag==CDIRQ_ACK&&response_count==1,"SCEx counter reset owns one status ACK");
    CHECK(cdrom_read(0x1f801801)==stat_reg,"SCEx counter reset answers the status");
    ack();
    cdrom_write(0x1f801800,0);cdrom_write(0x1f801802,0x05);command(0x19);
    CHECK(irq_flag==CDIRQ_ACK&&response_count==2,"SCEx counters own one two-byte ACK");
    CHECK(cdrom_read(0x1f801801)==0x00&&cdrom_read(0x1f801801)==0x00,"SCEx counters read zero, not the status");
    ack();
}
static void test_status(int source,int present) {
    set_model("PSX_CD_COLD_STATUS_MODEL",source?"octoshock-2.2.2":"");
    iso_handle=NULL;cdrom_init(present?"synthetic":NULL);
    command(1);
    CHECK(cdrom_read(0x1f801801)==(present?(source?0x12:0x02):0x10),"first GetStat reports cold latch before clearing");
    CHECK(irq_flag==CDIRQ_ACK,"GetStat ACK");ack();command(1);
    CHECK(cdrom_read(0x1f801801)==(present?0x02:0x10),"second GetStat clears only closed present-disc latch");
    ack();command(0x1a);
    CHECK((cdrom_read(0x1f801801)&0x10)==(present?0:0x10),"GetID sees resulting tray state");
}

/* A completed Stop must remain stopped when a source drive polls GetStat.
 * Exercise real MMIO/IRQ paths and the existing snapshot wire format. */
static void motor_issue(uint8_t c) {
    command(c);
    for(int n=0;queued_cmd.pending && n<32;n++) {
        uint64_t due=s_source_command_due>s_source_ready_due?s_source_command_due:s_source_ready_due;
        CHECK(due>=psx_cycle_count,"queued command deadline is not in the past");
        advance((int)(due-psx_cycle_count));
    }
    CHECK(!queued_cmd.pending,"motor test command received");
}
static void motor_complete(void) {
    CHECK(pending.pending,"motor transition has a second response");
    ack();advance((int)(pending.due_cyc-psx_cycle_count));
    CHECK(irq_flag==CDIRQ_COMPLETE,"motor transition completes normally");
}
static void motor_boot(const char *tape,const char *drive) {
    set_model("PSX_CD_SOURCE_CLOCK_TAPE",tape);
    set_model("PSX_CD_DRIVE_MODEL",drive);
    set_model("PSX_CD_CDDA_MODEL",*tape?"octoshock-2.3":"");
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL",*tape?"octoshock-2.2.2":"");
    set_model("PSX_CD_TOC_SEEK_MODEL",*tape?"octoshock-2.2.2":"");
    set_model("PSX_CD_READ_START_MODEL",*tape?"octoshock-2.2.2-pipeline":"");
    psx_cycle_count=0;i_stat=0;cdrom_init("synthetic");
}
static void test_motor_status(const char *tape,const char *drive) {
    motor_boot(tape,drive);
    motor_issue(8);
    CHECK(irq_flag==CDIRQ_ACK,"Stop acknowledges before completion");
    motor_complete();
    CHECK(!(stat_reg&CDSTAT_MOTOR) && cdrom_read(0x1f801801)==0,
          "completed Stop reports and stores motor off");
    unsigned size=cdrom_snapshot_bytes();uint8_t *wire=malloc(size);
    cdrom_snapshot_write(wire);ack();
    uint8_t expected=*drive?0:CDSTAT_MOTOR;
    for(int n=0;n<2;n++) {
        motor_issue(1);
        CHECK(cdrom_read(0x1f801801)==expected,"stopped GetStat reports preserved source motor state");
        CHECK((stat_reg&CDSTAT_MOTOR)==expected,"stopped GetStat preserves source motor state");
        ack();
    }
    CHECK(cdrom_snapshot_read(wire,size),"stopped motor snapshot restores without a new wire field");
    CHECK(!(stat_reg&CDSTAT_MOTOR),"snapshot retains stopped physical motor");ack();
    motor_issue(1);
    CHECK(cdrom_read(0x1f801801)==expected,"restored stopped GetStat preserves source status");ack();

    /* Only real start commands turn the source motor back on. */
    CHECK(cdrom_snapshot_read(wire,size),"restore before MotorOn");ack();
    motor_issue(7);motor_complete();
    CHECK(stat_reg&CDSTAT_MOTOR,"MotorOn completion starts the motor");ack();
    CHECK(cdrom_snapshot_read(wire,size),"restore before ReadN");ack();
    motor_issue(6);
    CHECK(stat_reg&CDSTAT_MOTOR,"ReadN starts the motor from stopped state");
    CHECK(cdrom_snapshot_read(wire,size),"restore before SeekL");ack();
    motor_issue(0x15);
    CHECK((stat_reg&(CDSTAT_MOTOR|CDSTAT_SEEK))==(CDSTAT_MOTOR|CDSTAT_SEEK),
          "SeekL starts the motor and seeks");
    CHECK(cdrom_snapshot_read(wire,size),"restore before Play");ack();
    motor_issue(3);
    CHECK(stat_reg==(CDSTAT_MOTOR|(source_cdda.enabled?CDSTAT_SEEK:CDSTAT_PLAY)),
          "Play starts the motor in its existing source-seek or legacy-play phase");
    free(wire);

    motor_boot(tape,drive);motor_issue(9);motor_complete();ack();
    motor_issue(1);
    CHECK(cdrom_read(0x1f801801)==CDSTAT_MOTOR && (stat_reg&CDSTAT_MOTOR),
          "Pause and ordinary spinning GetStat keep motor on");ack();
    motor_boot(tape,drive);iso_handle=NULL;stat_reg=CDSTAT_SHELL;
    motor_issue(1);
    CHECK(cdrom_read(0x1f801801)==CDSTAT_SHELL && !(stat_reg&CDSTAT_MOTOR),
          "no-disc GetStat keeps shell state without starting motor");
}

/* Shipped2.4.2 Chrono988277 opens with INT5 16h/08h then stops10h;
 * shipped2.8/Octoshock2.7 FFIX467782 opens without an unsolicited IRQ.
 * Both close into one-second startup, motor off, not-ready01h/80h.
 * Existing exact-provider measurements; real MMIO and route entry points. */
static uint8_t lid_getstat(void) {
    motor_issue(1);
    CHECK(irq_flag==CDIRQ_ACK,"route-lid GetStat acknowledges");
    uint8_t s=cdrom_read(0x1f801801);ack();return s;
}
static void test_route_lid_242(const char *tape,const char *drive) {
    set_model("PSX_CD_COLD_STATUS_MODEL","octoshock-2.2.2");
    motor_boot(tape,drive);
    motor_issue(9);motor_complete();ack();
    CHECK(stat_reg&CDSTAT_MOTOR,"paused drive spins before the swap");
    CHECK(cdrom_route_tray_open(),"route opens the tray");
    for(int n=0;n<100;n++) advance(1000);
    if(strcmp(drive,"octoshock-2.4.2")==0) {
        CHECK(irq_flag==CDIRQ_ERROR,"shipped2.4.2 tray open raises INT5");
        CHECK(cdrom_read(0x1f801801)==0x16 && cdrom_read(0x1f801801)==0x08,
              "shipped2.4.2 open response is16h/08h");
        CHECK(stat_reg==CDSTAT_SHELL,"shipped2.4.2 stops after the open response");
        ack();
    } else CHECK(irq_flag==0,"measured2.7 tray open raises no unsolicited interrupt");
    CHECK(!pending.pending,"tray open drops the pending second response");
    CHECK(lid_getstat()==CDSTAT_SHELL,"open-tray GetStat is shell only, motor stopped, no error");
    CHECK(cdrom_route_disc_select("disc2.cue",NULL),"route selects the next disc");
    uint64_t closed_at=psx_cycle_count;
    CHECK(cdrom_route_tray_close(),"route closes the tray");
    CHECK(lid_getstat()==CDSTAT_SHELL,"first closed GetStat: latched shell, motor still off");
    CHECK(lid_getstat()==0,"shell acknowledged; drive stopped during startup");
    motor_issue(0x15);
    CHECK(irq_flag==CDIRQ_ERROR && cdrom_read(0x1f801801)==CDSTAT_ERROR &&
          cdrom_read(0x1f801801)==0x80,"startup SeekL reports not-ready 01h/80h");ack();
    motor_issue(0x1A);
    CHECK(irq_flag==CDIRQ_ERROR && cdrom_read(0x1f801801)==CDSTAT_ERROR &&
          cdrom_read(0x1f801801)==0x80,"startup GetID reports not-ready 01h/80h");ack();
    CHECK(psx_cycle_count<closed_at+33868800ull,"checks ran inside the startup second");
    psx_cycle_count=closed_at+33868800ull-1;cdrom_advance(0);
    CHECK(!(stat_reg&CDSTAT_MOTOR),"startup does not finish early");
    advance(1);
    CHECK(stat_reg&CDSTAT_MOTOR,"startup finishes at the one-second deadline");
    CHECK(lid_getstat()==CDSTAT_MOTOR,"inserted disc then reports a spinning drive");
    set_model("PSX_CD_COLD_STATUS_MODEL","");
}

/* Exact retained2.10 ELF: Pause's -1 state still owns a motor notification.
 * Exercise actual tray/MMIO/advance paths, not a synthetic IRQ injection. */
static void test_route_lid_210(const char *tape) {
    set_model("PSX_CD_TRAY_MODEL","octoshock-2.10");
    set_model("PSX_CD_COLD_STATUS_MODEL","octoshock-2.2.2");
    motor_boot(tape,"octoshock-2.7");
    motor_issue(9);motor_complete();ack();advance(2000);
    CHECK(cdrom_route_tray_open(),"source210 paused tray opens");
    CHECK(irq_flag==0,"source210 physical open alone does not notify");
    advance(1);
    CHECK(irq_flag==CDIRQ_ERROR,"source210 paused update raises INT5");
    CHECK(cdrom_read(0x1f801801)==0x16 && cdrom_read(0x1f801801)==0x08,
          "source210 paused notification snapshots16h08h");
    CHECK(stat_reg==CDSTAT_SHELL,"source210 notification leaves live status10h");
    ack();advance(2000);
    CHECK(lid_getstat()==CDSTAT_SHELL,"source210 later GetStat sees10h not payload16h");

    motor_boot(tape,"octoshock-2.7");
    motor_issue(9);motor_complete();ack();advance(2000);
    motor_issue(1);
    CHECK(irq_flag==CDIRQ_ACK,"source210 preexisting GetStat IRQ held");
    uint8_t held_response=response_fifo[response_read];
    CHECK(cdrom_route_tray_open(),"source210 opens with a held IRQ");
    advance(1);
    CHECK(irq_flag==CDIRQ_ACK,"source210 does not overwrite held IRQ");
    CHECK(cdrom_read(0x1f801801)==held_response,"source210 preserves old response FIFO");
    unsigned bytes=cdrom_snapshot_bytes();uint8_t *wire=malloc(bytes);
    cdrom_snapshot_write(wire);
    ack();advance(2000);
    CHECK(cdrom_snapshot_read(wire,bytes),"source210 queued controller state restores");free(wire);
    CHECK(irq_flag==CDIRQ_ACK,"source210 queued notification snapshot restores held IRQ");
    ack();advance(1999);
    CHECK(irq_flag==0,"source210 queued notice waits full receive deadline");
    advance(1);
    CHECK(irq_flag==CDIRQ_ERROR,"source210 queued notice presents when ready");
    CHECK(cdrom_read(0x1f801801)==0x16 && cdrom_read(0x1f801801)==0x08,
          "source210 queued notification retains original motor payload");
    ack();

    motor_boot(tape,"octoshock-2.7");
    motor_issue(8);motor_complete();ack();advance(2000);
    CHECK(cdrom_route_tray_open(),"source210 stopped tray opens");
    advance(1);
    CHECK(irq_flag==0,"source210 stopped idle drive does not invent notification");
    set_model("PSX_CD_TRAY_MODEL","");
    set_model("PSX_CD_COLD_STATUS_MODEL","");
}

int main(int argc,char **argv) {
    for(int firmware=0;firmware<2;firmware++) for(int cold=0;cold<2;cold++) {
        set_model("PSX_CD_COLD_STATUS_MODEL",cold?"octoshock-2.2.2":"");test_version(firmware);
        test_scex_counters(firmware);
        for(int disc=0;disc<2;disc++) test_status(cold,disc);
    }
    set_model("PSX_CD_FIRMWARE_MODEL","");set_model("PSX_CD_COLD_STATUS_MODEL","");
    test_version(0);test_status(0,1);
    for(int source=0;source<2;source++) for(int fast=0;fast<2;fast++) {
        set_model("PSX_CD_TOC_SEEK_MODEL",source?"octoshock-2.2.2":"");
        cdrom_init("synthetic");psx_cycle_count=0;mode_reg=fast?0x80:0;
        command(0x1e);
        int expected=30000000+(source?(20000+(fast?1237952:2475904)):0);
        CHECK(pending.due_cyc==(uint64_t)expected,"TOC includes exact declared cold paused seek lower bound");
        CHECK(irq_flag==CDIRQ_ACK&&response_fifo[response_read]==2,"TOC first response remains pre-command ACK");
        ack();advance(expected-1);CHECK(irq_flag==0,"TOC cannot complete one cycle early");
        advance(1);CHECK(irq_flag==CDIRQ_COMPLETE,"TOC completes at declared deadline");
    }
    CHECK(source_seek_lower_bound(123,0,0,0,0)==33888800,"stopped motor restarts at zero");
    CHECK(source_seek_lower_bound(0,2250,1,1,0)==10395840,"long distance uses settle instead of paused term");
    CHECK(source_seek_lower_bound(2250,0,1,1,0)==10395840,"reverse travel uses same distance");
    CHECK(source_seek_lower_bound(0,4,1,0,0)==20000,"standby is not paused");
    set_model("PSX_CD_TOC_SEEK_MODEL","");
    if(argc!=2){fprintf(stderr,"synthetic clock fixture required\n");return 2;}
    test_motor_status("","");
    test_motor_status(argv[1],"");
    test_motor_status(argv[1],"octoshock-2.7");
    test_motor_status(argv[1],"nymashock-1.29.0");
    test_route_lid_242(argv[1],"octoshock-2.4.2");
    /* BizHawk 2.8 (octoshock-2.7 selector): FFIX 6726 swaps discs at return 467781. */
    test_route_lid_242(argv[1],"octoshock-2.7");
    test_route_lid_210(argv[1]);
    test_route_lid_242(argv[1],"octoshock-2.4.2");
    test_route_lid_242(argv[1],"octoshock-2.7");
    if(failures){fprintf(stderr,"FAILED (%d)\n",failures);return 1;}
    puts("CD boot model: ALL PASS");return 0;
}

#include "cdrom_iso_fixture_adapter.h"
