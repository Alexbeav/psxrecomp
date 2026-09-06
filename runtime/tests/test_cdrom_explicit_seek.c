/* Source-owned explicit seek regression. Includes the real controller to
 * exercise its command lifecycle with synthetic media and inert host edges. */
#include "../src/cdrom.c"
uint64_t psx_cycle_count;
uint32_t i_stat;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint64_t s_frame_count;

/* cdrom.c is compiled as one production translation unit. These inert host
 * edges satisfy its linker contract; the test does not execute them. */
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void event_ring_record(uint16_t kind, uint8_t detail) { (void)kind; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind; (void)a; (void)b;
}
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
void spu_cd_audio_push(const int16_t *stereo, int frames) {
    (void)stereo; (void)frames;
}
void spu_cd_audio_reset(void) {}
int psx_netplay_active(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }

void *iso_open(const char *path) { (void)path; return NULL; }
void iso_close(void *handle) { (void)handle; }
int iso_read_sector(void *handle, uint32_t lba, uint8_t *buffer, int size) {
    (void)handle; (void)lba; (void)buffer; (void)size; return 0;
}
int iso_read_raw_sector(void *handle, uint32_t lba, uint8_t *buffer, int size) {
    (void)handle; (void)lba; (void)buffer; (void)size; return 0;
}
int iso_read_subq(void *handle, uint32_t lba, uint8_t *buffer, int size,
                  int *valid) {
    (void)handle; (void)lba; (void)buffer; (void)size;
    if (valid) *valid = 0;
    return 0;
}
int iso_has_subq_replacements(void *handle) { (void)handle; return 0; }
uint32_t iso_sector_count(void *handle) { (void)handle; return 0; }
int iso_track_count(void *handle) { (void)handle; return 0; }
uint32_t iso_track_start_lba(void *handle, int track) {
    (void)handle; (void)track; return 0;
}
uint32_t iso_track_pregap_lba(void *handle, int track) {
    (void)handle; (void)track; return 0;
}
int iso_track_is_audio(void *handle, int track) {
    (void)handle; (void)track; return 0;
}


static int failures;
#define CHECK(x,label) do { if(!(x)){ fprintf(stderr,"FAIL: %s\n",label); ++failures; } } while(0)
static void seed_stream(int read_command) {
 cdrom_init(NULL); iso_handle=(void*)(uintptr_t)1;
 reading=1;read_cmd=read_command;read_delay=1234;
 read_min=0;read_sec=10;read_sect=0;
 seek_min=0;seek_sec=20;seek_sect=0;
 s_setloc_lba=msf_to_lba(0,20,0);setloc_pending=1;
 stat_reg=CDSTAT_MOTOR|CDSTAT_READ;mode_reg=0x80;
 s_cd_timing_next_due=1234;
 pending_dataready=1;pending_dataready_stat=CDSTAT_READ;pending_present_due=5678;
 request_reg=CDROM_REQUEST_BFRD;
 for(int i=0;i<CDROM_NUM_SECTOR_BUFFERS;i++){s_sector_ring[i].size=2048;s_sector_ring[i].pos=32;}
}
int main(void) {
 for(int r=0;r<2;r++)for(int s=0;s<2;s++) {
  int read_command=r?0x1b:0x06;int seek_command=s?0x16:0x15;
  seed_stream(read_command);
  exec_command(seek_command);
  CHECK(!reading && !read_cmd && !read_delay && !s_cd_timing_next_due,"explicit seek cancels old stream");
  CHECK(!pending_dataready && !pending_dataready_stat && !pending_present_due,"explicit seek cancels pending INT1");
  CHECK(!(request_reg&CDROM_REQUEST_BFRD),"explicit seek clears data request");
  for(int i=0;i<CDROM_NUM_SECTOR_BUFFERS;i++) CHECK(!s_sector_ring[i].size&&!s_sector_ring[i].pos,"explicit seek clears old sector slots");
  CHECK((stat_reg&(CDSTAT_READ|CDSTAT_PLAY|CDSTAT_SEEK))==CDSTAT_SEEK,"seek owns drive status");
  irq_flag=0;psx_cycle_count=pending.due_cyc;process_pending(0);
  CHECK(!reading,"completed seek remains stopped");
  irq_flag=0;exec_command(read_command);
  CHECK(reading && read_min==0 && read_sec==20 && read_sect==0,"next read begins at requested target");
  CHECK(read_delay==initial_read_delay_cycles(),"read keeps established first-sector timing");
 }
 seed_stream(0x1b);setloc_pending=0;irq_flag=0;
 exec_command(0x1b);
 CHECK(reading&&read_sec==10&&read_delay==1234,"repeated read without seek retains current stream");
 CHECK(s_sector_ring[0].size==2048&&s_sector_ring[0].pos==32,"repeated read preserves unread sector");
 if(failures)return 1;
 puts("PASS explicit seek ownership, target restart, unchanged read timing and repeated-read control");return 0;
}
