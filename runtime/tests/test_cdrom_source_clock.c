/* Reuse the authored controller stubs and default timing regression. */
#define main baseline_explicit_timing_main
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
        CHECK(due>=psx_cycle_count,"command stage deadline monotonic");
        advance((int)(due-psx_cycle_count));
    }
    CHECK(!queued_cmd.pending,"command admitted within phase bound");
}
int main(int argc,char **argv) {
    if(argc<2)return 2;
    tape_path=argv[1];
    if(argc==3) {
        clock_boot();
        if(!strcmp(argv[2],"late-argument")) {command(1);cdrom_write(0x1f801802,0);}
        else if(!strcmp(argv[2],"cdda"))command(3);
        else if(!strcmp(argv[2],"exhausted")) {s_source_clock_tape.cursor=s_source_clock_tape.count;command(1);}
        return 1; /* The invalid operation must have stopped the process. */
    }
    set_model("PSX_CD_SOURCE_CLOCK_TAPE","");
    CHECK(baseline_explicit_timing_main()==0,"old explicit timing controls remain passing");
    for(int size=0;size<=4;size++) {
        clock_boot();request_reg=CDROM_REQUEST_BFRD;RB_.pos=0;RB_.size=size;
        uint32_t expected=0;
        for(int i=0;i<size;i++){RB_.data[i]=(uint8_t)(0x10+i);expected|=(uint32_t)RB_.data[i]<<(i*8);}
        CHECK(cdrom_dma_read_padded()==expected && RB_.pos==size,"source padded DMA drains available bytes and zeros missing bytes");
        CHECK(cdrom_dma_read_padded()==0 && RB_.pos==size,"empty padded DMA remains empty");
        RB_.pos=0;request_reg=0;CHECK(cdrom_dma_read_padded()==0 && RB_.pos==0,"native BFRD ownership remains enforced");
        request_reg=CDROM_REQUEST_BFRD;
        if(size<4)CHECK(cdrom_dma_read()==0 && RB_.pos==0,"ordinary DMA partial-word behavior stays unchanged");
    }
    clock_boot();
    cdrom_write(0x1f801802,0x20);command(0x19);
    CHECK(s_source_clock_tape.cursor==3 && s_source_clock_calls==1,"source first command consumes rejection words");
    CHECK(s_source_command_due==13452 && !irq_flag,"initial reception period has source jitter");
    advance(13451);CHECK(!irq_flag && s_source_command_phase==-1,"no early reception phase");
    advance(1);CHECK(s_source_command_phase==0 && s_source_command_due==15267,"one parameter reception phase");
    advance(1815);CHECK(s_source_command_phase==1 && s_source_command_due==23767,"source final reception phase");
    advance(8499);CHECK(!irq_flag,"no early command response");
    advance(1);CHECK(irq_flag==3 && (i_stat&4) && psx_cycle_count==23767,"ACK flag and CPU line at actual command execution");

    clock_boot();command(1);receive_command();
    CHECK(psx_cycle_count==21952,"zero arguments omit parameter receive phase");
    command(1);advance(30000);
    CHECK(queued_cmd.pending && s_source_command_phase==-1,"old IRQ blocks the first reception stage");
    ack();uint64_t cleared=psx_cycle_count;
    advance(1999);CHECK(s_source_command_phase==-1 && !irq_flag,"post-ACK receive interval remains closed");
    advance(1);CHECK(s_source_command_phase==1 && !irq_flag,"receive resumes after full 2000 clocks");
    advance(8499);CHECK(!irq_flag,"later command execution delay is not skipped by blocked time");
    advance(1);CHECK(irq_flag==3 && psx_cycle_count-cleared==10500,"blocked command retains remaining source phase");

    clock_boot();cdrom_write(0x1f801802,0);cdrom_write(0x1f801802,2);cdrom_write(0x1f801802,4);command(2);
    command(1);receive_command();
    CHECK(!setloc_pending && s_source_clock_calls==2 && s_source_clock_tape.cursor==4,"replacement keeps command-write entropy order and cancels old arguments");
    CHECK(psx_cycle_count==20918,"replacement reception uses its own source jitter");

    clock_boot();cdrom_write(0x1f801802,0x20);command(0x19);advance(13452);
    unsigned length=cdrom_snapshot_bytes();uint8_t *wire=malloc(length),*before=malloc(length),*after=malloc(length);
    cdrom_snapshot_write(wire);
    receive_command();ack();command(1);receive_command();
    CHECK(s_source_clock_tape.cursor==4,"second command advances entropy before restore");
    CHECK(cdrom_snapshot_read(wire,length) && s_source_clock_tape.cursor==3 && s_source_command_phase==0,"controller snapshot restores cursor and reception stage");
    CHECK(s_source_command_due-psx_cycle_count==1815,"restored stage uses remaining delay");
    receive_command();ack();command(1);receive_command();
    CHECK(s_source_clock_tape.cursor==4,"restored branch consumes the same next word");
    unsigned offsets[]={4,36,40,44,48,52,56,60,64};
    for(unsigned n=0;n<sizeof(offsets)/sizeof(offsets[0]);n++) {
        cdrom_snapshot_write(before);uint8_t saved=wire[length-76+offsets[n]];
        memset(wire+length-76+offsets[n],0xff,offsets[n]==4?1:4);
        if(offsets[n]==48) { memset(wire+length-76+48,0,4);wire[length-76+48]=2; }
        CHECK(!cdrom_snapshot_read(wire,length),"bad tape identity or clock state rejected");
        cdrom_snapshot_write(after);CHECK(!memcmp(before,after,length),"invalid clock snapshot rejected before any controller mutation");
        /* Restore the valid source image for the next malformed case. */
        (void)saved;memcpy(wire,before,length);
    }
    free(wire);free(before);free(after);

    for(int paused=0;paused<=1;paused++) {
        clock_boot();mode_reg=0x80;s_source_seek_paused=(uint8_t)paused;
        read_min=seek_min=0;read_sec=seek_sec=2;read_sect=seek_sect=4;setloc_pending=0;
        command(6);receive_command();
        int expected=677376+20000+4996+(paused?1237952:0);
        CHECK(read_delay==expected,"fresh ReadN seeks from standby without a pending Setloc, then fills three periods");
        CHECK(s_source_clock_tape.cursor==5 && s_source_clock_calls==2,"ReadN consumes command and seek entropy separately");
        ack();advance(expected-1);CHECK(!irq_flag && reads==0,"no early first sector");
        advance(1);CHECK(irq_flag==1 && reads==1,"first sector appears at the declared source clock deadline");
    }

    clock_boot();command(1);receive_command();ack();
    reading=1;read_cmd=6;read_min=0;read_sec=2;read_sect=4;mode_reg=0x80;read_delay=1000;
    s_cd_timing_next_due=psx_cycle_count+1000;
    advance(1000);CHECK(!irq_flag && pending_dataready,"sector can arrive while receive interval is closed");
    advance(999);CHECK(!irq_flag,"pended sector stays hidden until receive interval ends");
    advance(1);CHECK(irq_flag==1,"pended sector is admitted when receive interval reopens");
    clock_boot();CHECK(s_source_clock_tape.cursor==0 && s_source_clock_calls==0,"cold boot resets tape cursor and call count");
    if(failures){fprintf(stderr,"source clock failures %d\n",failures);return 1;}
    puts("CD source clock: ALL PASS");return 0;
}
