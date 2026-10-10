/* Controller-level regression for the drive-profile rows of the behaviour spec
 * (recomp-corpus references/ps1/CDROM-SOURCE-PROFILE-SPEC.md) that the older
 * tests do not pin: S1-S6, R1, P1, P3, P4, P5 at single speed and at a large
 * position, K2 with a late acknowledge, K3, T2, G2c, G3, G5, I3, I5, I7 and
 * I11. The expected numbers follow from the spec rows and cd_seek_delay.h.
 * Synthetic controller data only; no BIOS or game data.
 *
 * usage: cdrom_drive_profile_rows_test <tape>
 *        cdrom_drive_profile_rows_test <tape> bad-model|no-tape|init-cdda|init-read-seek
 * Each named mode must stop with exit status 2 and a [CDROM] message. */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

#define PERIOD_1X 451584u
#define PERIOD_2X 225792u

static const char *tape_path;

static void select_drive(const char *model) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE",tape_path);
    set_model("PSX_CD_DRIVE_MODEL",model);
    psx_cycle_count=0;cdrom_init("synthetic");
}
/* Every draw from an all-zero tape is 0 and consumes one word. */
static void zero_tape(unsigned words) {
    free(s_source_clock_tape.bytes);
    s_source_clock_tape.bytes=calloc(words,4);
    s_source_clock_tape.count=words;s_source_clock_tape.cursor=0;
}
static void cursor_at(int lba) { lba_to_msf(lba,150,&read_min,&read_sec,&read_sect); }
static int cursor(void) { return msf_to_lba(read_min,read_sec,read_sect); }
static void run(uint8_t cmd) { irq_flag=0;response_clear();exec_command(cmd); }
static void setloc(int lba) {
    int n=lba+150;
    param_fifo[0]=bin_to_bcd(n/4500);param_fifo[1]=bin_to_bcd(n/75%60);
    param_fifo[2]=bin_to_bcd(n%75);param_count=3;run(0x02);
}
static int getlocp(void) {
    run(0x11);
    if(response_count!=8)return -100000;
    return (bcd_to_bin(response_fifo[5])*60+bcd_to_bin(response_fifo[6]))*75+
           bcd_to_bin(response_fifo[7])-150;
}
static void present_pending(void) { irq_flag=0;s_source_ready_due=0;process_pending(0); }

static void selection_rows(void) {
    set_model("PSX_CD_DRIVE_MODEL","");set_model("PSX_CD_SOURCE_CLOCK_TAPE","");
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","");set_model("PSX_CD_TOC_SEEK_MODEL","");
    cdrom_init("synthetic");
    CHECK(!s_nymashock_drive && !s_cd_reset_seek_draw_first,"S1: empty selector selects no drive profile");
    source_drive_head_valid=1;source_drive_head_lba=77;source_drive_head_target=78;
    source_drive_head_due=79;source_drive_hold_logical=1;source_reset_phase=3;
    source_drive_subq_lba=80;
    select_drive("nymashock-1.29.0");
    CHECK(s_nymashock_drive==1 && s_cd_reset_seek_draw_first==0,"S2: nymashock-1.29.0 takes the floor draw first");
    CHECK(!source_drive_head_valid && source_drive_head_lba==0 && source_drive_head_target==0 &&
          source_drive_head_due==0 && !source_drive_hold_logical && !source_reset_phase &&
          source_drive_subq_lba==-1,"S5: start state of the seven drive values");
    select_drive("nymashock-1.32.1");
    CHECK(s_nymashock_drive==1 && s_cd_reset_seek_draw_first==0,"BizHawk 2.10+ selector admits the existing candidate drive");
    select_drive("octoshock-2.7");
    CHECK(s_nymashock_drive==1 && s_cd_reset_seek_draw_first==1,"S3: octoshock-2.7 takes the jitter draw first");
}

static void read_and_pause_rows(void) {
    select_drive("nymashock-1.29.0");zero_tape(64);
    stat_reg=CDSTAT_MOTOR;s_source_seek_paused=0;mode_reg=0x80;psx_cycle_count=1000;
    source_drive_head_valid=1;source_drive_head_lba=994;source_drive_head_target=1000;
    source_drive_head_due=UINT64_MAX;source_drive_hold_logical=0;
    setloc(1000);run(0x06);
    CHECK(reading && source_drive_hold_logical==1 && !source_drive_head_valid,
          "R1/R2: a read start sets the hold flag and takes the head out of the idle model");
    CHECK(cursor()==1000 && s_source_read_start_lba==1000,"read start records the stream start");

    /* Eight sectors delivered, read established. */
    stat_reg=CDSTAT_MOTOR|CDSTAT_READ;cursor_at(1008);last_sector_lba=1007;
    CHECK(getlocp()==1009,"G2b: established read names the delivery cursor + 1");
    stat_reg=CDSTAT_MOTOR|CDSTAT_SEEK;
    CHECK(getlocp()==1008,"G2c: a read still seeking names the delivery cursor");
    stat_reg=CDSTAT_MOTOR|CDSTAT_READ;

    source_drive_subq_lba=777;psx_cycle_count=5000000;
    uint32_t draws=s_source_clock_tape.cursor;
    run(0x09);
    CHECK(pending.due_cyc-psx_cycle_count==1134106 && s_source_clock_tape.cursor==draws+1,
          "P5/P6: 1,124,584 + 1006*42596/4500 at 2x plus one draw");
    CHECK(response_fifo[0]==(CDSTAT_MOTOR|CDSTAT_READ),"P2: first response keeps the read bit");
    CHECK(cursor()==1006,"P3: delivery cursor moves back two sectors");
    CHECK(source_drive_head_valid && source_drive_head_lba==1006 && source_drive_head_target==1006 &&
          source_drive_head_due==psx_cycle_count+PERIOD_2X,"P4: head armed at the paused position");
    CHECK(source_drive_hold_logical==1 && source_drive_subq_lba==777,"P4: hold flag and last decoded position kept");
    CHECK(getlocp()==777,"G5: before the first advance GetlocP names the earlier decoded position");
    psx_cycle_count+=PERIOD_2X;CHECK(getlocp()==1006,"G3/H1: first decode is the paused position");
    psx_cycle_count+=PERIOD_2X;CHECK(getlocp()==1007,"G3/H2: window top is target + 1");
    psx_cycle_count+=PERIOD_2X;CHECK(getlocp()==999,"G3/H2: window bottom is target - 7");

    /* Short Pause: nothing reads or plays. */
    int valid=source_drive_head_valid,lba=source_drive_head_lba,anchor=source_drive_head_target;
    uint64_t due=source_drive_head_due;int at=cursor();
    draws=s_source_clock_tape.cursor;
    run(0x09);
    CHECK(pending.due_cyc-psx_cycle_count==5000 && s_source_clock_tape.cursor==draws,
          "P1: short Pause is 5,000 cycles and takes no draw");
    CHECK(source_drive_head_valid==valid && source_drive_head_lba==lba && source_drive_head_target==anchor &&
          source_drive_head_due==due && source_drive_hold_logical==1 && source_drive_subq_lba==999 &&
          cursor()==at,"P1: short Pause changes no drive value");

    /* Single speed, and the stream start as the lower limit. */
    reading=1;stat_reg=CDSTAT_MOTOR|CDSTAT_READ;mode_reg=0;
    s_source_read_start_lba=1000;cursor_at(1001);
    run(0x09);
    CHECK(pending.due_cyc-psx_cycle_count==2268098,"P5: whole value doubled at single speed");
    CHECK(cursor()==1000 && source_drive_head_lba==1000 && source_drive_head_due==psx_cycle_count+PERIOD_1X,
          "P3/P4: not below the stream start; next advance one 1x period later");

    /* Spec question Q2: exact arithmetic above sector 50,415. */
    reading=1;stat_reg=CDSTAT_MOTOR|CDSTAT_READ;mode_reg=0x80;
    s_source_read_start_lba=185000;cursor_at(185252);
    run(0x09);
    CHECK(pending.due_cyc-psx_cycle_count==2878119 && source_drive_head_lba==185250,
          "P5: exact product at sector 185,250");

    /* Long Pause with nothing delivered: position -1, latency as for 0. */
    reading=0;stat_reg=CDSTAT_MOTOR|CDSTAT_READ;last_sector_lba=-1;
    run(0x09);
    CHECK(pending.due_cyc-psx_cycle_count==1124584 && source_drive_head_lba==-1 &&
          source_drive_head_target==-1,"P4/P5: head takes the position before the lower limit");

    /* Default path: same formula, no draw, no drive value, cursor kept. */
    cd_tape_free(&s_source_clock_tape);s_source_clock=0;s_nymashock_drive=0;
    source_drive_head_valid=0;reading=1;stat_reg=CDSTAT_MOTOR|CDSTAT_READ;mode_reg=0x80;
    cursor_at(1000);
    CHECK(pause_complete_delay_cycles()==1134049 && cursor()==1000 && !source_drive_head_valid,
          "P5: default path at 2x");
    mode_reg=0;
    CHECK(pause_complete_delay_cycles()==2268098,"P5: default path doubled at single speed");
    reading=0;stat_reg=CDSTAT_MOTOR;
    CHECK(pause_complete_delay_cycles()==5000,"P1: default path short Pause");
}

static void seek_and_toc_rows(void) {
    select_drive("nymashock-1.29.0");zero_tape(64);
    stat_reg=CDSTAT_MOTOR;s_source_seek_paused=0;mode_reg=0;psx_cycle_count=1000;
    source_drive_hold_logical=0;
    setloc(2000);run(0x15);
    uint64_t done=pending.due_cyc;
    CHECK(!source_drive_head_valid,"seek in progress: head not valid");
    /* The first response stays unacknowledged for three sector periods. */
    psx_cycle_count=done+3*PERIOD_1X+10;process_pending(0);
    CHECK(pending.pending && !source_drive_head_valid,"second response waits for the acknowledge");
    present_pending();
    CHECK(irq_flag==CDIRQ_COMPLETE && source_drive_head_valid && source_drive_hold_logical==1 &&
          source_drive_head_target==2000,"K2: idle at the target with the hold flag set");
    CHECK(source_drive_head_lba==1997 && source_drive_subq_lba==1996 &&
          source_drive_head_due==done+4*PERIOD_1X,
          "K2: advances count from the scheduled completion, not from the acknowledge");

    setloc(3000);run(0x16);
    psx_cycle_count=pending.due_cyc;present_pending();
    CHECK(irq_flag==CDIRQ_COMPLETE && !source_drive_head_valid,"K3: SeekP does not start the head model");

    source_drive_head_valid=1;source_drive_head_lba=source_drive_head_target=2000;
    source_drive_hold_logical=1;source_drive_head_due=psx_cycle_count+PERIOD_1X;
    pending_dataready=1;s_source_seek_paused=0;
    run(0x1E);
    CHECK(source_drive_head_valid && source_drive_head_lba==2000 && source_drive_head_target==0 &&
          !source_drive_hold_logical && !pending_dataready && s_source_seek_paused,
          "T2/T3: window anchored to sector 0, hold flag clear, pended notice dropped");
    psx_cycle_count+=PERIOD_1X;source_drive_head_update();
    CHECK(source_drive_head_lba==1992 && source_drive_subq_lba==2000,"H3: head drifts down eight per period");
}

static void init_rows(void) {
    select_drive("nymashock-1.29.0");zero_tape(64);
    void *disc=iso_handle;iso_handle=NULL;
    stat_reg=CDSTAT_SHELL;mode_reg=0x80;cd_muted=1;setloc_pending=1;s_setloc_lba=1234;psx_cycle_count=1000;
    run(0x0A);
    CHECK(pending.due_cyc-psx_cycle_count==70000 && response_fifo[0]==CDSTAT_SHELL && stat_reg==CDSTAT_SHELL,
          "I3: no disc, second response after 70,000 cycles, status kept");
    CHECK(mode_reg==0x20 && !cd_muted && !setloc_pending && s_setloc_lba==0,
          "I2: demute, mode 20h, Setloc cancelled and its stored sector 0");
    CHECK(!s_source_reset_due && !source_reset_phase && !source_drive_head_valid &&
          s_source_clock_tape.cursor==0,"I3: no travel and no draw");
    /* No disc while the head travels: the stage-1 answer comes first. */
    source_reset_phase=1;s_source_reset_due=psx_cycle_count+999999;mode_reg=0x80;
    run(0x0A);
    CHECK(pending.due_cyc-psx_cycle_count==256 && s_source_reset_due==psx_cycle_count+999999 &&
          mode_reg==0x20 && s_source_clock_tape.cursor==0,
          "I9 before I3: stage 1 without a disc is answered after 256 cycles");
    source_reset_phase=0;s_source_reset_due=0;
    iso_handle=disc;

    reading=0;stat_reg=CDSTAT_MOTOR;s_source_seek_paused=0;cursor_at(5000);
    source_drive_head_valid=0;source_drive_hold_logical=0;source_drive_subq_lba=4321;
    psx_cycle_count=10000000;
    run(0x0A);
    CHECK(s_source_reset_due==psx_cycle_count+10683306 && s_source_clock_tape.cursor==2,
          "I5/I6: without a head model the travel starts at the delivery cursor");
    CHECK(pending.due_cyc==psx_cycle_count+4100000,"I8: second response has its own deadline");
    CHECK(source_drive_head_valid && source_drive_head_lba==0 && source_drive_head_target==0 &&
          source_reset_phase==1 && !source_drive_hold_logical && source_drive_subq_lba==4321,
          "I7: head at sector 0, stage 1, hold flag and decoded position kept");
    CHECK((stat_reg&(CDSTAT_SEEK|CDSTAT_MOTOR))==(CDSTAT_SEEK|CDSTAT_MOTOR) && !s_source_seek_paused,
          "I4: seeking with the motor on, not paused");
    uint64_t command_time=psx_cycle_count;
    psx_cycle_count+=5*PERIOD_1X;source_drive_head_update();
    CHECK(source_drive_head_lba==0 && source_drive_subq_lba==4321,"I7: head does not advance during travel");

    psx_cycle_count=command_time+4100000;present_pending();
    CHECK(pending.pending && irq_flag==0 && pending.due_cyc==s_source_reset_due,
          "I11: reply put off to the reset deadline");
    psx_cycle_count=s_source_reset_due-100;pending.due_cyc=psx_cycle_count;present_pending();
    CHECK(pending.pending && irq_flag==0 && pending.due_cyc==psx_cycle_count+256,
          "I11: never less than 256 cycles ahead");
    psx_cycle_count=s_source_reset_due;process_source_reset();
    CHECK(!s_source_reset_due && !source_reset_phase && source_drive_head_lba==-8,
          "I13/I14: hold flag clear, reset ends at arrival with the head at -8");
    psx_cycle_count=pending.due_cyc;present_pending();
    CHECK(irq_flag==CDIRQ_COMPLETE && !pending.pending,"I11/I12: reply presented once the reset is over");
    psx_cycle_count+=3*PERIOD_1X;
    CHECK(getlocp()==0 && source_drive_subq_lba==-7,
          "G3/G4: a decoded position below sector 0 is not reported; the delivery cursor is");
}

int main(int argc,char **argv) {
    if(argc<2)return 2;
    tape_path=argv[1];
    if(argc==3) {
        if(!strcmp(argv[2],"bad-model"))select_drive("default");
        else if(!strcmp(argv[2],"no-tape")) {
            set_model("PSX_CD_SOURCE_CLOCK_TAPE","");set_model("PSX_CD_DRIVE_MODEL","nymashock-1.29.0");
            cdrom_init("synthetic");
        } else if(!strcmp(argv[2],"init-cdda")) {
            select_drive("nymashock-1.29.0");zero_tape(8);cdda_playing=1;run(0x0A);
        } else if(!strcmp(argv[2],"init-read-seek")) {
            select_drive("nymashock-1.29.0");zero_tape(8);
            reading=1;stat_reg=CDSTAT_MOTOR|CDSTAT_SEEK;run(0x0A);
        }
        return 1;
    }
    selection_rows();
    read_and_pause_rows();
    seek_and_toc_rows();
    init_rows();
    if(failures){fprintf(stderr,"drive profile rows: %d failures\n",failures);return 1;}
    puts("CD drive profile rows: ALL PASS");return 0;
}
