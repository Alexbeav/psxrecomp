/* SCD2 context, corruption and atomic rejection. Authored state only; reachable
 * command/PCM continuation is separately covered by the complete transcripts. */
#include "cdrom_source_fixture_support.c"
static unsigned checks;
#define EXPECT(c,m) do{checks++;CHECK(c,m);}while(0)
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_TOC_SEEK_MODEL","octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL","octoshock-2.2.2-pipeline");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE",argv[1]);
    set_model("PSX_CD_DRIVE_MODEL","nymashock-1.29.0");
    set_model("PSX_CD_CDDA_MODEL","octoshock-2.3");
    psx_cycle_count=1234567;cdrom_init("synthetic");
    s_source_clock_tape.cursor=7;s_source_clock_calls=3;
    s_source_command_due=psx_cycle_count+123;s_source_ready_due=psx_cycle_count+99;
    s_source_reset_due=psx_cycle_count+500;s_source_command_phase=0;s_source_args_remaining=2;
    s_source_read_start_lba=100;s_source_seek_paused=1;
    pending_present_due=psx_cycle_count+77;pending.due_cyc=psx_cycle_count+88;
    cdrom_irq_present_due=psx_cycle_count+66;s_cd_timing_next_due=psx_cycle_count+55;
    source_drive_head_valid=1;source_drive_head_lba=10;source_drive_head_target=16;
    source_drive_head_due=psx_cycle_count+111;source_drive_hold_logical=1;
    source_reset_phase=2;source_drive_subq_lba=9;
    source_cdda.seeking=1;source_cdda.position_valid=1;source_cdda.play_track_match=2;
    source_cdda.sectors_read=37;source_cdda.pipe_count=2;source_cdda.pipe_at=1;
    source_cdda.report_last_tens=5;source_cdda.async_type=1;source_cdda.async_count=8;
    for(unsigned i=0;i<sizeof source_cdda.pipe;i++)((uint8_t*)source_cdda.pipe)[i]=(uint8_t)(i*17u);
    memset(source_cdda.async_data,0x5a,8);read_has_sector=1;
    s_lid.close_due=psx_cycle_count+777;s_lid.physical_open=1;s_lid.shell_open_latched=1;s_lid_irq_pending=1;
    uint32_t n=cdrom_snapshot_bytes(),base=cdrom_snapshot_base_bytes(),extra=source_snapshot_bytes();
    EXPECT(extra==4917u,"SCD2 includes the two pending tray-notification bytes");
    uint8_t *saved=malloc(n),*again=malloc(n),*bad=malloc(n);
    if(!saved||!again||!bad)abort();
    cdrom_snapshot_write(saved);
    s_source_clock_tape.cursor=0;s_source_clock_calls=0;s_source_command_due=0;s_source_ready_due=0;
    s_source_reset_due=0;s_source_command_phase=-1;s_source_args_remaining=0;
    source_drive_head_valid=0;source_drive_head_lba=123;source_drive_head_due=0;
    memset(source_cdda.pipe,0,sizeof source_cdda.pipe);source_cdda.async_type=0;source_cdda.async_count=0;
    source_cdda.pipe_count=source_cdda.pipe_at=0;s_source_seek_paused=0;read_has_sector=0;
    s_lid.close_due=0;s_lid.physical_open=0;s_lid.shell_open_latched=0;
    EXPECT(cdrom_snapshot_read(saved,n),"complete optional context accepted");
    cdrom_snapshot_write(again);EXPECT(!memcmp(saved,again,n),"all default and optional fields restore byte exactly");
    const struct{unsigned offset,bytes;uint32_t value;} corrupt[]={
        {8,1,2},{53,1,2},{54,1,2},{55,4,2},{115,4,257},{119,4,257},{123,4,257},
        {127,4,2},{131,4,17},{135,4,2},{139,4,2},{143,4,256},{151,4,3},{155,4,2},
        {159,4,16},{4875,4,6},{4879,4,9},{4883,4,2},{4887,4,450001},
        {4891,4,450001},{4903,4,2},{4907,4,4},{4911,4,(uint32_t)-151}
    };
    for(unsigned i=0;i<sizeof corrupt/sizeof corrupt[0];i++) {
        memcpy(bad,saved,n);uint8_t *field=bad+base+corrupt[i].offset;
        if(corrupt[i].bytes==1)*field=(uint8_t)corrupt[i].value;else put32(field,corrupt[i].value);
        EXPECT(!cdrom_snapshot_read(bad,n),"corrupt optional field refused");
        cdrom_snapshot_write(again);EXPECT(!memcmp(saved,again,n),"source refusal leaves default/optional fields intact");
    }
    const unsigned mismatches[]={0,4,83};
    for(unsigned i=0;i<3;i++) {
        memcpy(bad,saved,n);bad[base+mismatches[i]]^=0x80;
        EXPECT(!cdrom_snapshot_read(bad,n),"marker/profile/tape identity mismatch refused");
        cdrom_snapshot_write(again);EXPECT(!memcmp(saved,again,n),"identity refusal atomic");
    }
    EXPECT(!cdrom_snapshot_read(saved,n-1),"truncated optional state refused");
    EXPECT(!cdrom_snapshot_read(saved,base),"profile-less state refused in enabled profile");
    /* CDF6 adds9 bytes after CDF5's12-byte gain section. The CDF4
     * received-sector Boolean remains immediately before those sections. */
    memcpy(bad,saved,n);bad[base-22u]=2;
    EXPECT(!cdrom_snapshot_read(bad,n),"modern CDF4 receipt validation still applies");
    cdrom_snapshot_write(again);EXPECT(!memcmp(saved,again,n),"CDF4 refusal remains atomic with source appendix");
    free(saved);free(again);free(bad);
    printf("source SCD2 context: %u checks, %d failures\n",checks,failures);
    return failures!=0;
}
