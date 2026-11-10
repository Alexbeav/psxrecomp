/* Authored controller controls. Synthetic sectors, no retail media or guest. */
#define main original_explicit_timing_main
#include "test_cdrom_source_explicit_timing.c"
#undef main
static int checks, cases;
static void verify(int ok,const char *name){checks++;if(!ok){fprintf(stderr,"CONTROL FAIL %d: %s\n",cases,name);failures++;}}
static void init_control(const char *tape,int drive){
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_COLD_STATUS_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");
    set_model("PSX_CD_CDDA_MODEL","octoshock-2.3");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE",tape);
    set_model("PSX_CD_DRIVE_MODEL",drive?"octoshock-2.7":"");
    psx_cycle_count=1000000;cdrom_init("synthetic");
    stat_reg=CDSTAT_MOTOR;mode_reg=1;s_source_seek_paused=0;
    lba_to_msf(126600,150,&read_min,&read_sec,&read_sect);
    int m,s,f;lba_to_msf(126600,150,&m,&s,&f);seek_min=(uint8_t)m;seek_sec=(uint8_t)s;seek_sect=(uint8_t)f;
    s_setloc_lba=126600;setloc_pending=1;
}
static void set_next_word(uint32_t word){
    uint32_t i=s_source_clock_tape.cursor*4;
    verify(i+4<=s_source_clock_tape.count*4,"tape word available");
    for(int n=0;n<4;n++)s_source_clock_tape.bytes[i+n]=(uint8_t)(word>>(8*n));
}
/* A completed SeekP hands playback to the standby head: the transport starts
 * from the sector the seek selected, so a following GetlocP reports that
 * small back-step rather than the last sector that was read. */
/* Reduced physical-position projection of the exact source transition. The
 * actual controller below executes commands and response presentation. This
 * projection excludes sector I/O, pipes, IRQs, and command acceptance. */
static int reference_head(int target,uint64_t arrival,uint64_t now,uint64_t period){
    int cur=target-9;if(cur<-150)cur=-150;
    cur++;if(cur>=target)cur=cur-9<-150?-150:cur-9;
    uint64_t next=arrival+2*period;
    while(next<=now){cur++;if(cur>=target)cur=cur-9<-150?-150:cur-9;next+=period;}
    return cur;
}
/* With no argument, write a synthetic zero tape (64 words) beside the test. */
static const char *synthetic_tape(void){
    static const char *path="cdrom_source_seekp_play.tape";
    uint8_t header[20]={0};memcpy(header,"PSX-CD-RNG1",11);header[16]=64;
    FILE *f=fopen(path,"wb");if(!f)return NULL;
    uint8_t zero[256]={0};int ok=fwrite(header,1,20,f)==20&&fwrite(zero,1,256,f)==256;
    return fclose(f)==0&&ok?path:NULL;
}
int main(int argc,char **argv){
    const char *tape_arg[2]={argv[0],argc==2?argv[1]:synthetic_tape()};
    if(argc>2||!tape_arg[1])return 2;
    argv=(char **)tape_arg;
    const int phases[]={0,1,2,3,8,9,10,11,18,23};
    for(unsigned i=0;i<sizeof(phases)/sizeof(phases[0]);i++){
        cases++;init_control(argv[1],1);exec_command(0x16);
        uint64_t due=pending.due_cyc,period=source_drive_sector_period();
        verify(pending.pending&&due==1020000,"actual SeekP lower-bound deadline");
        psx_cycle_count=due-1;process_pending(0);
        verify(!source_drive_head_valid,"no standby head before physical arrival");
        psx_cycle_count=due+(uint64_t)phases[i]*period;
        int want=reference_head(126600,due,psx_cycle_count,period);
        process_pending(0);/* original ACK deliberately still held */
        verify(pending.pending&&irq_flag==CDIRQ_ACK,"held ACK retains only response presentation");
        verify(source_drive_head_valid&&source_drive_head_lba==want,"physical idle head progresses while ACK is held");
        verify(!(stat_reg&CDSTAT_SEEK),"physical arrival clears seeking before response delivery");
        irq_flag=0;s_source_ready_due=0;process_pending(0);
        verify(!pending.pending&&irq_flag==CDIRQ_COMPLETE,"second response is presented after ACK");
        verify(setloc_pending==1,"explicit SeekP retains SetLoc dirty until Play");
        irq_flag=0;set_next_word(13436);uint32_t cursor=s_source_clock_tape.cursor,calls=s_source_clock_calls;
        exec_command(3);
        const CDROMSourceCDDAPlayDebugState *p=&s_source_cdda_play_debug;
        verify(p->valid&&p->new_seek&&p->origin_lba==want&&p->target_lba==126600,"Play uses physical origin and retained target");
        verify(p->setloc_pending==1&&!setloc_pending,"Play consumes dirty only when starting its own seek");
        int lower=psx_cd_seek_delay(want,126600,1,0,1,1);
        verify(p->lower_bound_cycles==lower&&p->jitter_cycles==13436&&p->delay_cycles==(uint32_t)(lower+13436),"Play retains exact seek formula and one measured-kind jitter");
        verify(s_source_clock_tape.cursor==cursor+1&&s_source_clock_calls==calls+1,"one draw; no extra random calls");
        verify(!source_drive_head_valid,"Play retires idle-head model before active playback");
        if(phases[i]==3)verify(want==126594&&p->delay_cycles==1839772,"original measured head inputs yield source1839772 rather than native33436");
        printf("CASE %d phase=%d head=%d origin=%d target=%d dirty=%d lower=%d jitter=%u delay=%u\n",cases,phases[i],want,p->origin_lba,p->target_lba,p->setloc_pending,p->lower_bound_cycles,p->jitter_cycles,p->delay_cycles);
    }
    cases++;init_control(argv[1],1);exec_command(0x16);uint64_t due=pending.due_cyc;
    psx_cycle_count=due-1;set_next_word(13436);irq_flag=0;exec_command(3);
    verify(s_source_cdda_play_debug.origin_lba==126600&&s_source_cdda_play_debug.delay_cycles==33436,"Play before physical arrival uses source PreSeekHack logical cursor, not future standby head");
    cases++;init_control(argv[1],0);exec_command(0x16);psx_cycle_count=pending.due_cyc;irq_flag=0;process_pending(0);
    verify(!source_drive_head_valid&&!setloc_pending,"non-drive profile retains its original explicit-seek state");
    set_next_word(13436);irq_flag=0;exec_command(3);
    verify(s_source_cdda_play_debug.origin_lba==126600&&s_source_cdda_play_debug.delay_cycles==33436,"neutral profile retains original Play origin/delay");
    const int edges[]={-150,-149,-142,-141,-1,0,4};
    for(unsigned n=0;n<sizeof(edges)/sizeof(edges[0]);n++){
        cases++;init_control(argv[1],1);int m,s,f;
        lba_to_msf(edges[n],150,&m,&s,&f);seek_min=(uint8_t)m;seek_sec=(uint8_t)s;seek_sect=(uint8_t)f;
        s_setloc_lba=edges[n];exec_command(0x16);uint64_t arrival=pending.due_cyc,period=source_drive_sector_period();
        psx_cycle_count=arrival;process_pending(0);
        verify(source_drive_head_valid&&source_drive_head_lba==reference_head(edges[n],arrival,psx_cycle_count,period),"lead-in clamp preserves immediate standby step");
        psx_cycle_count=arrival+3*period;process_pending(0);source_drive_head_update();
        verify(source_drive_head_lba==reference_head(edges[n],arrival,psx_cycle_count,period),"lead-in clamp preserves later nine-sector walk");
    }
    printf("RESULT cases=%d assertions=%d failures=%d\n",cases,checks,failures);
    return failures?1:0;
}
