/* PS1B-189: CD-ROM status register bit 7 (BUSYSTS), driven through the
 * register interface.
 *
 * PSX-SPX a253f078 docs/cdromdrive.md: 0x1F801800 bit 7 BUSYSTS "HC05 busy
 * acknowledging command"; "BUSYSTS flag": 0 = ready for a new command,
 * 1 = busy sending command/parameters; a new command sent before the previous
 * interrupt is acknowledged stays in the busy phase until it is.
 * Case: Getstat raises INT3; a second Getstat written before the ack waits,
 * so BUSYSTS reads 1; after the ack it runs and BUSYSTS reads 0. */
#include "../src/cdrom.c"
#include <stdio.h>
#include <stdlib.h>

/* Device seams cdrom.c links against. Only iso_open and the IRQ line are
 * reached (no disc is mounted); the rest abort if ever called. */
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t s_frame_count, psx_cycle_count;
#define UNREACHED abort()
void *iso_open(const char *p) { (void)p; return NULL; }
int iso_read_sector(void *h, uint32_t l, uint8_t *b, int n) { (void)h; (void)l; (void)b; (void)n; UNREACHED; }
int iso_read_raw_sector(void *h, uint32_t l, uint8_t *b, int n) { (void)h; (void)l; (void)b; (void)n; UNREACHED; }
int iso_read_subq(void *h, uint32_t l, uint8_t *b, int n, int *v) { (void)h; (void)l; (void)b; (void)n; (void)v; UNREACHED; }
int iso_has_subq_replacements(void *h) { (void)h; UNREACHED; }
uint32_t iso_sector_count(void *h) { (void)h; UNREACHED; }
void iso_close(void *h) { (void)h; }
int iso_track_count(void *h) { (void)h; UNREACHED; }
uint32_t iso_track_start_lba(void *h, int t) { (void)h; (void)t; UNREACHED; }
uint32_t iso_track_pregap_lba(void *h, int t) { (void)h; (void)t; UNREACHED; }
int iso_track_is_audio(void *h, int t) { (void)h; (void)t; UNREACHED; }
static unsigned irq_raises;
void psx_irq_raise(uint32_t b, uint32_t d) { (void)b; (void)d; ++irq_raises; }
uint32_t debug_guest_ra(void) { return 0; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
int dma_cdrom_transfer_active(void) { return 0; }
void event_ring_record(uint16_t k, uint8_t d) { (void)k; (void)d; }
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) { (void)k; (void)d; (void)a; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int psx_netplay_active(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
void spu_cd_audio_push(const int16_t *s, int f) { (void)s; (void)f; }
void spu_cd_audio_reset(void) { }

static unsigned checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}
static unsigned busysts(void) { return (cdrom_read(0x1F801800) >> 7) & 1u; }
static void run_until_irq(unsigned limit)
{
    for (unsigned i = 0; i < limit && !(irq_flag & 7u); ++i) { psx_cycle_count += 64; cdrom_tick(); }
}

int main(void)
{
    cdrom_init(NULL);
    check(busysts() == 0, "idle drive: BUSYSTS 0");

    cdrom_write(0x1F801800, 0);                  /* bank 0 */
    cdrom_write(0x1F801801, 0x01);               /* Getstat */
    run_until_irq(1000000);
    check((irq_flag & 7u) != 0, "first Getstat raised its interrupt");
    check(busysts() == 0, "first command taken: BUSYSTS 0");

    cdrom_write(0x1F801800, 0);
    cdrom_write(0x1F801801, 0x01);               /* second Getstat, interrupt not acknowledged */
    check(busysts() == 1, "command waiting behind an unacknowledged interrupt: BUSYSTS 1");

    cdrom_write(0x1F801800, 1);                  /* bank 1: acknowledge all interrupt flags */
    cdrom_write(0x1F801803, 0x1F);
    for (unsigned i = 0; i < 1000000 && busysts(); ++i) { psx_cycle_count += 64; cdrom_tick(); }
    check(busysts() == 0, "queued command started: BUSYSTS 0");

    printf("cdrom BUSYSTS (PSX-SPX): %u checks passed\n", checks);
    return 0;
}
