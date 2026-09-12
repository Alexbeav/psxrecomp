#define main old_cdda_main
#include "test_cdrom_source_cdda.c"
#undef main
#include <assert.h>
int main(void) {
    for (int clock_profile=0;clock_profile<2;clock_profile++) {
    s_source_clock=clock_profile;
    source_cdda.enabled = 1;
    source_cdda.seeking = 0; source_cdda.position_valid = 1;
    source_cdda.play_track_match = -1; source_cdda.sectors_read = 123;
    source_cdda.pipe_count = 2; source_cdda.pipe_at = 1;
    source_cdda.report_last_tens = 255;
    source_cdda.async_type = 1; source_cdda.async_count = 8;
    for (unsigned i=0;i<sizeof source_cdda.pipe;i++)
        ((uint8_t*)source_cdda.pipe)[i] = (uint8_t)(i*17+3);
    memset(source_cdda.async_data, 0x4c, sizeof source_cdda.async_data);
    memset(cd_pending_vol, 0x37, sizeof cd_pending_vol);
    cd_decode_vol[0][0]=0x40;cd_decode_vol[0][1]=0x20;
    cd_decode_vol[1][0]=0x10;cd_decode_vol[1][1]=0x60;
    if (clock_profile) {
        psx_cycle_count=1000;pending.due_cyc=17;cdrom_irq_present_due=21;
        s_source_command_due=23;s_source_ready_due=29;s_source_reset_due=0;
        pending_present_due=31;s_cd_timing_next_due=37;
        s_ring_read=3;s_ring_write=7;pending_dataready_slot=2;
        for (unsigned slot=0;slot<8;slot++) {
            memset(s_sector_ring[slot].data,(int)slot+41,SECTOR_BUFFER_SIZE);
            s_sector_ring[slot].size=2048;s_sector_ring[slot].pos=(int)slot*11;
        }
    }
    uint32_t n=cdrom_snapshot_bytes();
    uint8_t *wire=malloc(n), *again=malloc(n); assert(wire && again);
    cdrom_snapshot_write(wire);
    memset(&source_cdda,0,sizeof source_cdda);source_cdda.enabled=1;
    memset(cd_pending_vol,0,sizeof cd_pending_vol);
    memset(cd_decode_vol,0,sizeof cd_decode_vol);
    if (clock_profile) {memset(s_sector_ring,0,sizeof s_sector_ring);s_ring_read=s_ring_write=0;}
    assert(cdrom_snapshot_read(wire,n));
    {
        int16_t pcm[2]={1000,-2000};
        cd_apply_decode_volume(pcm,1);
        assert(pcm[0]==250 && pcm[1]==-1250);
        assert(cd_pending_vol[1][0]==0x37);
    }
    assert(source_cdda.pipe_count==2 && source_cdda.pipe_at==1);
    assert(source_cdda.play_track_match==-1 && source_cdda.async_count==8);
    if (clock_profile) {
        assert(s_ring_read==3 && s_ring_write==7 && pending_dataready_slot==2);
        assert(s_sector_ring[7].data[100]==48 && s_sector_ring[7].pos==77);
        assert(pending.due_cyc==17 && s_source_ready_due==29 && s_cd_timing_next_due==37);
    }
    cdrom_snapshot_write(again);assert(!memcmp(wire,again,n));
    free(wire);free(again);
    }
    puts("CDDA pipeline, sector ring and absolute deadlines round trip passes");
    return 0;
}
