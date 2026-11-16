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

static void check_open_status(uint8_t expected) {
    /* FFVII original 2.6.2 return396559: response buffer is {0x16,0x08}.
     * Drive the production route/MMIO path; no response replacement in fixture. */
    stat_reg=CDSTAT_MOTOR|CDSTAT_READ;reading=1;irq_flag=0;irq_enable=7;
    response_clear();
    CHECK(cdrom_route_tray_open(),"physical route opens once");
    cdrom_write(0x1f801800,1);
    CHECK((cdrom_read(0x1f801803)&7)==CDIRQ_ERROR,"lid interrupt keeps encoded reason5");
    cdrom_write(0x1f801800,0);
    CHECK(cdrom_read(0x1f801801)==expected,"original lid-open status byte through response FIFO");
    CHECK(cdrom_read(0x1f801801)==0x08,"lid-open reason byte remains08");
    CHECK(!reading&&!cdrom_lid_media_ready(&s_lid,1),"open lid stops reads and hides media");
    uint64_t closed_at=psx_cycle_count;
    CHECK(cdrom_route_tray_close(),"explicit route closes without a synthetic delay");
    ack();
    if (s_cd_262_lid_status) {
        command(0x01);
        for (int n=0;n<1000 && irq_flag!=CDIRQ_ACK;n++) advance(1000);
        CHECK(irq_flag==CDIRQ_ACK,"closed-tray GetStat reaches its real ACK");
        cdrom_write(0x1f801800,0);
        uint8_t closed_status=cdrom_read(0x1f801801);
        CHECK(!(closed_status&CDSTAT_SEEKERR),
              "closed 2.6.2 tray clears lid seek-error through GetStat FIFO");
        CHECK(!(closed_status&CDSTAT_MOTOR),"inserted 2.6.2 disc stays stopped during startup");
        ack();
        command(0x08);
        for (int n=0;n<1000 && !irq_flag;n++) advance(1000);
        cdrom_write(0x1f801800,0);
        CHECK(irq_flag==CDIRQ_ERROR && cdrom_read(0x1f801801)==CDSTAT_ERROR &&
              cdrom_read(0x1f801801)==0x80,"startup Stop reports original not-ready error0180");
        ack();
        unsigned size=cdrom_snapshot_bytes();uint8_t *wire=malloc(size);
        uint64_t deadline=s_lid.close_due;uint8_t status=stat_reg;
        cdrom_snapshot_write(wire);s_lid.close_due=0;stat_reg^=CDSTAT_MOTOR;
        CHECK(cdrom_snapshot_read(wire,size) && s_lid.close_due==deadline && stat_reg==status,
              "closed startup deadline and status survive existing checkpoint wire");
        free(wire);
        psx_cycle_count=closed_at+33868800ull-1;cdrom_advance(0);
        CHECK(!(stat_reg&CDSTAT_MOTOR),"inserted disc does not finish startup early");
        advance(1);
        CHECK(stat_reg&CDSTAT_MOTOR,"inserted disc finishes startup at original clock deadline");
    }
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
int main(int argc,char **argv) {
    if(argc==3) {
        /* Original 2.4.2/2.6.2 movie-state observations show two header
         * intervals and a physical standby head, not the older single-slot
         * adapter. Test named admission through production initialization,
         * then the existing deterministic real-MMIO controls without jitter. */
        set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
        set_model("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");
        set_model("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");
        set_model("PSX_CD_SOURCE_CLOCK_TAPE",argv[1]);
        set_model("PSX_CD_DRIVE_MODEL",argv[2]);
        cdrom_init("synthetic");
        CHECK(s_nymashock_drive,"named original host enables staged physical drive");
        if(!strcmp(argv[2],"octoshock-2.6.2")) {
            check_open_status(0x16);
            cdrom_init("synthetic");
        }
        s_source_clock=0;psx_cycle_count=0;target(4);command(0x15);
        CHECK(pending.due_cyc==3399072,"original host SeekL has two header intervals");
        ack();advance(3399071);CHECK(irq_flag==0,"no completion before second header");
        advance(1);CHECK(irq_flag==CDIRQ_COMPLETE,"completion at second header deadline");
        CHECK(source_drive_head_valid&&source_drive_head_lba==-2,"physical standby head bound");
        cd_tape_free(&s_source_clock_tape);
        printf("Original host profile %s: failures=%d\n",argv[2],failures);
        return failures?1:0;
    }

    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","");cdrom_init("synthetic");
    check_open_status(0x11);
    cdrom_init("synthetic");
    unsigned default_size=cdrom_snapshot_bytes();
    uint8_t *default_a=malloc(default_size),*default_b=malloc(default_size);
    cdrom_snapshot_write(default_a);s_source_seek_paused=0;cdrom_snapshot_write(default_b);
    CHECK(memcmp(default_a,default_b,default_size),"default wire carries paused/standby state");
    CHECK(cdrom_snapshot_read(default_a,default_size)&&s_source_seek_paused==1,"default snapshot restores paused state");
    unsigned legacy_size=cdrom_snapshot_legacy_bytes();
    uint8_t *legacy=malloc(legacy_size);PstW legacy_writer;
    pst_w_init(&legacy_writer,legacy,legacy_size);
    CHECK(cdrom_snap_emit(&legacy_writer,0)&&legacy_writer.written==legacy_size,
          "authored legacy default state has its documented length");
    CHECK(cdrom_snapshot_read(legacy,legacy_size),"unselected default still reads legacy state");
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    for(int fast=0;fast<2;fast++) for(int logical=0;logical<2;logical++) for(int paused=0;paused<2;paused++)
        printf("fast=%d logical=%d paused=%d delay=%d\n",fast,logical,paused,run_seek(fast,logical,paused));
    cdrom_init("synthetic");psx_cycle_count=0;target(4);
    unsigned size=cdrom_snapshot_bytes();CHECK(size==default_size,"profile shares the default timing byte");
    uint8_t *wire=malloc(size);cdrom_snapshot_write(wire);
    CHECK(source_snapshot_flags()==8u && source_snapshot_bytes()==0,
          "explicit-seek-only selector exercises the no-appendix identity boundary");
    CHECK(!cdrom_snapshot_validate(legacy,legacy_size),"selected model refuses profileless legacy state");
    CHECK(!cdrom_snapshot_read(legacy,legacy_size),"selected legacy read refuses before mutation");
    cdrom_snapshot_write(default_b);
    CHECK(!memcmp(wire,default_b,size),"profileless refusal preserves all controller state");
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
    uint32_t head_size=cdrom_snapshot_bytes();uint8_t *head_wire=malloc(head_size);
    cdrom_snapshot_write(head_wire);source_drive_head_lba=123;
    CHECK(cdrom_snapshot_read(head_wire,head_size) && source_drive_head_lba==-1,"physical head survives same-profile state encoding");
    head_wire[head_size-32]=2;source_drive_head_lba=123;
    CHECK(!cdrom_snapshot_read(head_wire,head_size) && source_drive_head_lba==123,"invalid physical-head flag fails before mutation");
    free(head_wire);
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
    /* Draw order is the selector's (spec row I6): tape {3000000, 7}. nymashock-1.29.0
     * takes the floor draw first (max(3000000, 2495904+7)); octoshock-2.7 takes the
     * seek-jitter draw first (3000000&0x7FFF=18112). */
    for(int jitter_first=0;jitter_first<2;jitter_first++) {
        uint8_t *words=s_source_clock_tape.bytes;memset(words,0,16);
        words[0]=0xC0;words[1]=0xC6;words[2]=0x2D;words[4]=7;
        s_source_clock_tape.cursor=0;s_cd_reset_seek_draw_first=jitter_first;
        reading=0;stat_reg=CDSTAT_MOTOR;s_source_seek_paused=1;source_reset_phase=0;
        source_drive_head_valid=1;source_drive_head_lba=9;psx_cycle_count=100;irq_flag=0;
        exec_command(0x0A);
        CHECK(s_source_reset_due==100+(jitter_first?2495904+18112:3000000) && s_source_clock_tape.cursor==2,
              jitter_first?"octoshock-2.7 Init takes the seek-jitter draw first":"nymashock-1.29.0 Init takes the floor draw first");
    }
    s_cd_reset_seek_draw_first=0;
    cdrom_init("synthetic");s_nymashock_drive=1;
    reading=1;stat_reg=CDSTAT_MOTOR|CDSTAT_READ;
    lba_to_msf(1234,150,&read_min,&read_sec,&read_sect);
    response_clear();irq_flag=0;exec_command(0x11);
    const uint8_t physical_q[8]={1,1,0,0x16,0x35,0,0x18,0x35};
    CHECK(response_count==8 && !memcmp(response_fifo,physical_q,8),"Nymashock GetlocP reports the last physical read before the data pipeline");
    s_nymashock_drive=0;response_clear();irq_flag=0;exec_command(0x11);
    CHECK(response_fifo[4]==0x34 && response_fifo[7]==0x34,"older GetlocP position remains unchanged");
    cd_tape_free(&s_source_clock_tape);s_source_clock=0;s_nymashock_drive=0;

    /* Vagrant original 2.9.1 RAM captures: SeekL has completed at3014;
     * Pause is still pending3016..3018, then Read starts3019. Native used
     * the already-paused fast path from STANDBY and finished in5000 cycles.
     * Exercise the existing seek/Pause MMIO path and unchanged settle law. */
    cdrom_init("synthetic");psx_cycle_count=0;s_nymashock_drive=1;mode_reg=0x80;
    target(85000);command(0x15);finish();
    CHECK(!s_source_seek_paused,"completed SeekL is standby, not already paused");
    cd_tape_free(&s_source_clock_tape);
    s_source_clock_tape.bytes=calloc(2,4);s_source_clock_tape.count=2;
    CHECK(s_source_clock_tape.bytes!=NULL,"Pause fixture tape available");
    s_source_clock=1;advance(3*225792);response_clear();irq_flag=0;exec_command(0x09);
    uint64_t pause_wait=pending.due_cyc-psx_cycle_count;
    /* Existing head contract puts it at84997 after three advances. Original
     * 2.9.1 has two prefetched logical headers, so Pause retreats to84995;
     * observed pending counter3018=318958, not318977. */
    uint64_t standby_wait=1124584+(uint64_t)84995*42596/4500;
    CHECK(pause_wait==standby_wait,"standby Pause backs out both prefetched logical headers");
    CHECK(s_source_clock_tape.cursor==1,"standby Pause consumes its existing single settle draw");
    finish();response_clear();irq_flag=0;exec_command(0x09);
    CHECK(pending.due_cyc-psx_cycle_count==5000 && s_source_clock_tape.cursor==1,
          "repeated already-paused Pause remains fast without a new draw");
    finish();cd_tape_free(&s_source_clock_tape);s_source_clock=0;s_nymashock_drive=0;

    /* Chrono4538 original2.4.2 at988390: SeekTarget=-6 and remaining
     * Pause counter160124; native clamping to zero leaves160180. The
     * existing settle draw is80416. Exercise real command scheduling,
     * including zero/positive positions and both speed multipliers. */
    const int pause_positions[] = {-6, 0, 4};
    const int pause_settles[] = {1124528, 1124584, 1124621};
    for (int fast=0; fast<2; fast++) for (int n=0; n<3; n++) {
        cdrom_init("synthetic");psx_cycle_count=1000;
        s_nymashock_drive=1;s_source_clock=1;mode_reg=fast?0x80:0;
        s_source_clock_tape.bytes=calloc(1,4);s_source_clock_tape.count=1;
        CHECK(s_source_clock_tape.bytes!=NULL,"signed Pause tape available");
        uint32_t pause_draw=80416;
        memcpy(s_source_clock_tape.bytes,&pause_draw,4);
        s_source_clock_tape.cursor=0;
        reading=0;stat_reg=CDSTAT_MOTOR;s_source_seek_paused=0;
        source_drive_head_valid=1;source_drive_head_lba=pause_positions[n]+2;
        source_drive_head_target=0;source_drive_head_due=UINT64_MAX;
        source_drive_hold_logical=1;response_clear();irq_flag=0;
        exec_command(0x09);
        uint64_t expected=(uint64_t)pause_settles[n]*(fast?1:2)+pause_draw;
        CHECK(pending.due_cyc-psx_cycle_count==expected,
              "physical Pause preserves signed lead-in slope and speed");
        CHECK(source_drive_head_target==pause_positions[n],
              "physical Pause keeps its measured stop position");
        CHECK(s_source_clock_tape.cursor==1,"signed Pause consumes exactly one settle draw");
        cd_tape_free(&s_source_clock_tape);s_source_clock=0;s_nymashock_drive=0;
    }

    if(argc==4 && !strcmp(argv[1],"--extension")) {
        cdrom_init("synthetic");
        CHECK(cd_tape_load(&s_source_clock_tape,argv[2]),"original tape loads");
        s_source_clock=1;
        uint32_t original_count=s_source_clock_tape.count;
        s_source_clock_tape.cursor=original_count-1;s_source_clock_calls=1;
        unsigned ext_size=cdrom_snapshot_bytes();uint8_t *ext_wire=malloc(ext_size);
        cdrom_snapshot_write(ext_wire);
        CHECK(cd_tape_load(&s_source_clock_tape,argv[3]),"extended tape loads");
        int restored=cdrom_snapshot_read(ext_wire,ext_size);
        CHECK(restored && s_source_clock_tape.cursor==original_count-1,
              "production CD snapshot accepts exact-prefix extension and restores cursor");
        if(restored) {
            uint32_t value;
            CHECK(cd_tape_bounded(&s_source_clock_tape,UINT32_MAX,&value),"last old word remains available");
            CHECK(cd_tape_bounded(&s_source_clock_tape,UINT32_MAX,&value) && s_source_clock_tape.cursor==original_count+1,
                  "restored controller continues past old tape end");
        }
        uint32_t held=s_source_clock_tape.cursor,stat=stat_reg;
        s_source_clock_tape.bytes[0]^=1;
        CHECK(!cdrom_snapshot_read(ext_wire,ext_size) && s_source_clock_tape.cursor==held && stat_reg==stat,
              "changed prefix rejected before CD state mutation");
        free(ext_wire);cd_tape_free(&s_source_clock_tape);s_source_clock=0;
        set_model("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");
        set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
        set_model("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");
        set_model("PSX_CD_SOURCE_CLOCK_TAPE",argv[2]);
        set_model("PSX_CD_SOURCE_CLOCK_TAPE_EXTENSION",argv[3]);
        cdrom_init("synthetic");
        CHECK(s_source_clock && s_source_clock_tape.count>original_count && !s_source_clock_tape.cursor,
              "production initialization loads verified longer prefix without advancing cursor");
        cd_tape_free(&s_source_clock_tape);s_source_clock=0;
    }
    free(legacy);free(wire);free(default_a);free(default_b);
    if(failures){fprintf(stderr,"FAILED (%d)\n",failures);return 1;}
    puts("CD source explicit timing: ALL PASS");return 0;
}

#include "cdrom_iso_fixture_adapter.h"
