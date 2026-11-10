/* Synthetic behavioral bridge: production route session -> production CD-ROM
 * event functions -> real ISOReader. No BIOS, game, movie or retail media. */
#include "../src/cdrom.c"
#include "input_route_session.h"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
static uint8_t psx_ram[2 * 1024 * 1024];
uint8_t *g_psx_ram = psx_ram;
static int failures, checks, callback_count;

uint32_t debug_guest_ra(void) { return 0; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }
void event_ring_record(uint16_t a, uint8_t b) { (void)a; (void)b; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void audio_trace_event(uint16_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
void spu_cd_audio_push(const int16_t *p, int n) { (void)p; (void)n; }
void spu_cd_audio_reset(void) {}
int psx_netplay_active(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
void mod_runtime_patch_disc_sector(uint32_t lba, int raw, uint8_t *data, uint32_t size)
{ (void)lba; (void)raw; (void)data; (void)size; }

void sio_set_multitap(int enabled) { (void)enabled; }
void sio_set_pad_connected(int slot, int value) { (void)slot; (void)value; }
void sio_set_pad_config_capable(int slot, int value) { (void)slot; (void)value; }
void sio_set_pad_sticks(int slot, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
{ (void)slot; (void)lx; (void)ly; (void)rx; (void)ry; }
void sio_set_pad_analog(int slot, int enabled, uint8_t ly, uint8_t lx, uint8_t ry, uint8_t rx)
{ (void)slot; (void)enabled; (void)ly; (void)lx; (void)ry; (void)rx; }
void sio_set_pad_state_slot(int slot, uint16_t buttons) { (void)slot; (void)buttons; }

#define CHECK(c, m) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

static int apply_event(uint8_t event, uint8_t disc_index, const char *path,
                       char *why, size_t why_size)
{
    uint32_t before[4],after[4];char actual[4096]={0};
    extern int iso_source_bin_path(void *,char *,uint32_t);
    ++callback_count;cdrom_route_observe(before);
    int result=cdrom_route_apply_event(event,path,"SCEA",why,(uint32_t)why_size);
    cdrom_route_observe(after);
    int present=iso_source_bin_path(cdrom_route_media_handle(),actual,sizeof actual);
    CHECK(input_route_session_event_receipt(event,disc_index,result,s_frame_count,psx_cycle_count,
                                          before,after,actual,present,why),"actual committed event receipt flushes");
    return result;
}

static int mounted_byte(uint8_t expected)
{
    uint8_t sector[2048];
    return iso_handle && iso_read_sector(iso_handle, 0, sector, sizeof(sector)) &&
           sector[0] == expected && sector[2047] == expected;
}

int main(int argc, char **argv)
{
    if (argc != 6 || (strcmp(argv[1], "success") && strcmp(argv[1], "mutate"))) {
        fprintf(stderr, "usage: test success|mutate route disc1 disc2 mutate-target\n");
        return 2;
    }
    const char *roster[2] = {argv[3], argv[4]};
    cdrom_init(argv[3]);
    CHECK(mounted_byte(0x11), "disc 1 is initially mounted through the real reader");
    input_route_session_set_event_executor(apply_event);
    CHECK(input_route_session_set_disc_roster(roster, 2), "ordered roster configured");
    CHECK(input_route_session_admit(argv[2]), "valid synthetic event route admitted");
    CHECK(input_route_session_boundary() == -1 && callback_count == 1,
          "frame 0 opens the tray once");

    s_frame_count=1;
    if (!strcmp(argv[1], "mutate")) {
        FILE *f = fopen(argv[5], "ab");
        CHECK(f && fwrite("changed", 1, 7, f) == 7 && fclose(f) == 0,
              "selected media mutates after admission");
        CHECK(input_route_session_boundary() == 2, "changed selected media fails at boundary");
        CHECK(callback_count == 1, "changed media invokes no select or close callback");
        CHECK(input_route_session_boundary() == -1 && callback_count == 1,
              "disarmed route does not retry");
        CHECK(cdrom_route_tray_close(), "fixture can close the still-open original disc");
        CHECK(mounted_byte(0x11), "refusal leaves disc 1 mounted and readable");
    } else {
        CHECK(input_route_session_boundary() == -1 && callback_count == 3,
              "frame 1 selects then closes in file order");
        CHECK(mounted_byte(0x22), "disc 2 is mounted and readable through the real reader");
        CHECK(input_route_session_boundary() == -1 && callback_count == 3,
              "completed events execute once");
    }
    printf("input_route_cdrom_events: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}


int mod_runtime_read_disc_extent(uint32_t lba,int raw,uint8_t *p,uint32_t n) {(void)lba;(void)raw;(void)p;(void)n;return 0;}
uint32_t mod_runtime_disc_extent_start(void) {return 0;}
uint32_t mod_runtime_disc_sector_count(uint32_t base) {return base;}
int mod_runtime_cdda_track_count(void) {return 0;}
uint32_t mod_runtime_cdda_track_start(int track) {(void)track;return 0;}
int mod_runtime_read_cdda_sector(uint32_t lba,uint8_t *p,uint32_t n,uint32_t *source) {(void)lba;(void)p;(void)n;(void)source;return 0;}
void savestate_set_disc_scope(int disc) {(void)disc;}
