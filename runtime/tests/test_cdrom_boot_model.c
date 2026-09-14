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
int main(void) {
    for(int firmware=0;firmware<2;firmware++) for(int cold=0;cold<2;cold++) {
        set_model("PSX_CD_COLD_STATUS_MODEL",cold?"octoshock-2.2.2":"");test_version(firmware);
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
    if(failures){fprintf(stderr,"FAILED (%d)\n",failures);return 1;}
    puts("CD boot model: ALL PASS");return 0;
}
