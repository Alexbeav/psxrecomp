/* Source-profile 0A reset contract. Expected deadlines/state come from the
 * original 2.2.2 Command_Reset/update path and passive 699..708 state capture.
 * Only synthetic controller data. No title, BIOS or restored game state. */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

static const char *tape_path;
static void clock_boot(void) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE",tape_path);
    psx_cycle_count=0;cdrom_init("synthetic");i_stat=0;reads=irq_raises=0;
}
static void receive_command(void) {
    for(int guard=0;queued_cmd.pending && guard<20;guard++) {
        CHECK(!irq_flag,"fixture command must be eligible");
        uint64_t due=s_source_command_due>s_source_ready_due?s_source_command_due:s_source_ready_due;
        advance((int)(due-psx_cycle_count));
    }
    CHECK(!queued_cmd.pending,"command admitted within phase bound");
}

static uint64_t begin_reset(void) {
    clock_boot();
    mode_reg=0x80; read_min=0;read_sec=2;read_sect=10;
    seek_min=0;seek_sec=2;seek_sect=5;s_setloc_lba=5;
    cd_muted=1;command(0x0a);receive_command();
    CHECK(irq_flag==3,"reset acknowledges before drive completion");
    CHECK(mode_reg==0x80 && read_sect==10,"ACK keeps old head/mode");
    return psx_cycle_count;
}
static void check_reset_state(void) {
    CHECK(mode_reg==0x20 && read_min==0 && read_sec==2 && read_sect==0,
          "reset completes with source mode and physical LBA zero");
    CHECK(seek_min==0 && seek_sec==2 && seek_sect==0 && s_setloc_lba==0,
          "reset clears command location to LBA zero");
    CHECK(!cd_muted && s_source_seek_paused,"reset completes demuted and paused");
}
int main(int argc,char **argv) {
    if(argc<2)return 2;tape_path=argv[1];
    if(argc==3) {
        begin_reset();ack();
        if(!strcmp(argv[2],"during-reset")) {command(6);receive_command();}
        else if(!strcmp(argv[2],"active-read")) {clock_boot();reading=1;read_cmd=6;read_delay=2000000;command(10);receive_command();}
        return 1;
    }
    uint64_t start=begin_reset();ack();
    advance(485399);CHECK(mode_reg==0x80 && read_sect==10 && !irq_flag,"source frame700 retained state");
    advance(563976);CHECK(mode_reg==0x80 && read_sect==10 && !irq_flag,"source frame701 retained state");
    advance(86624);CHECK(mode_reg==0x80 && !irq_flag,"no early reset completion");
    advance(1);CHECK(psx_cycle_count-start==1136000 && irq_flag==2,"completion at exact source reset deadline");
    check_reset_state();ack();

    start=begin_reset();ack();advance(300000);
    command(0x0a);receive_command();ack();
    advance((int)(start+1135999-psx_cycle_count));
    CHECK(mode_reg==0x80 && !irq_flag,"repeated reset never completes early");
    advance(1);CHECK(irq_flag==2,"repeated reset does not rearm physical deadline");check_reset_state();

    start=begin_reset();ack();advance(300000);
    command(1);receive_command();ack();
    advance((int)(start+1136000-psx_cycle_count));
    CHECK(irq_flag==2,"GetStat does not cancel independent drive reset");check_reset_state();

    begin_reset();advance(1136000);
    CHECK(irq_flag==3,"busy ACK remains owner at drive completion");check_reset_state();
    ack();advance(2000);CHECK(!irq_flag,"source discards transient blocked reset completion");
    begin_reset();advance(1135999);ack();advance(1);
    CHECK(!irq_flag,"receive cooldown also blocks transient reset completion");check_reset_state();
    advance(2000);CHECK(!irq_flag,"no delayed stale reset completion after cooldown");

    begin_reset();ack();advance(485399);
    unsigned n=cdrom_snapshot_bytes();uint8_t *wire=malloc(n),*before=malloc(n),*after=malloc(n);
    cdrom_snapshot_write(wire);advance(650601);check_reset_state();ack();
    CHECK(cdrom_snapshot_read(wire,n),"active reset snapshot round trip");
    CHECK(mode_reg==0x80 && read_sect==10,"snapshot restores pre-completion state");
    advance(650600);CHECK(!irq_flag && mode_reg==0x80,"snapshot preserves remaining reset delay");
    advance(1);CHECK(irq_flag==2,"snapshot restored deadline completes once");check_reset_state();
    cdrom_snapshot_write(before);
    memset(wire+n-4,0xff,4);CHECK(!cdrom_snapshot_read(wire,n),"invalid reset remaining rejected");
    cdrom_snapshot_write(after);CHECK(!memcmp(before,after,n),"bad reset snapshot changes no state");
    free(wire);free(before);free(after);
    begin_reset();clock_boot();advance(1200000);CHECK(!irq_flag && mode_reg==0,"cold reset clears active drive reset");

    set_model("PSX_CD_SOURCE_CLOCK_TAPE","");psx_cycle_count=0;cdrom_init("synthetic");
    mode_reg=0x80;read_sec=2;read_sect=10;command(10);ack();
    advance(131071);CHECK(!irq_flag,"default OpenBIOS accommodation remains unchanged");
    advance(1);CHECK(irq_flag==2 && mode_reg==0x80 && read_sect==10,"default does not acquire source reset model");
    if(failures){fprintf(stderr,"source reset failures: %d\n",failures);return 1;}
    puts("CD source reset: ALL PASS");return 0;
}
