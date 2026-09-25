/* PS1B-118 / T172: the implicit seek of ReadN/ReadS travels from the drive's
 * current position, not from the last data sector delivered.
 *
 * Spec (PS1B-118, from PSX-SPX Setloc 02h, SeekL 15h, SeekP 16h, ReadN 06h,
 * ReadS 1Bh, GetlocP 11h):
 *   S3 after SeekL/SeekP the position is the seek target (no data delivered);
 *   S4 with no seek or read since reset the position is LBA 0;
 *   S5 a Setloc equal to the position gives zero travel.
 * Cases (a)-(c) run in the default (release) path. Each compares the charged
 * cycles with the seek model's own charge for the spec's origin, so only the
 * origin choice is under test. */
#include "../src/cdrom.c"
#include <stdio.h>
#include <stdlib.h>

/* Device seams cdrom.c links against. None is reached by these cases; each
 * aborts if it ever is. */
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t s_frame_count, psx_cycle_count;
#define UNREACHED abort()
void *iso_open(const char *p) { (void)p; UNREACHED; }
int iso_read_sector(void *h, uint32_t l, uint8_t *b, int n) { (void)h; (void)l; (void)b; (void)n; UNREACHED; }
int iso_read_raw_sector(void *h, uint32_t l, uint8_t *b, int n) { (void)h; (void)l; (void)b; (void)n; UNREACHED; }
int iso_read_subq(void *h, uint32_t l, uint8_t *b, int n, int *v) { (void)h; (void)l; (void)b; (void)n; (void)v; UNREACHED; }
int iso_has_subq_replacements(void *h) { (void)h; UNREACHED; }
uint32_t iso_sector_count(void *h) { (void)h; UNREACHED; }
void iso_close(void *h) { (void)h; UNREACHED; }
int iso_track_count(void *h) { (void)h; UNREACHED; }
uint32_t iso_track_start_lba(void *h, int t) { (void)h; (void)t; UNREACHED; }
uint32_t iso_track_pregap_lba(void *h, int t) { (void)h; (void)t; UNREACHED; }
int iso_track_is_audio(void *h, int t) { (void)h; (void)t; UNREACHED; }
void psx_irq_raise(uint32_t b, uint32_t d) { (void)b; (void)d; UNREACHED; }
uint32_t debug_guest_ra(void) { UNREACHED; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; UNREACHED; }
int dma_cdrom_transfer_active(void) { UNREACHED; }
void event_ring_record(uint16_t k, uint8_t d) { (void)k; (void)d; UNREACHED; }
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) { (void)k; (void)d; (void)a; UNREACHED; }
uint32_t interrupts_get_cycles_since_vblank(void) { UNREACHED; }
int psx_netplay_active(void) { UNREACHED; }
uint32_t psx_netplay_sim_tick(void) { UNREACHED; }
int psx_netplay_is_resimulating(void) { UNREACHED; }
int psx_netplay_cd_bisect_active(void) { UNREACHED; }
void spu_cd_audio_push(const int16_t *s, int f) { (void)s; (void)f; UNREACHED; }
void spu_cd_audio_reset(void) { UNREACHED; }

static unsigned checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

/* Drive state for the default path: motor on, not paused, single speed. */
static void drive(int cursor_lba, int last_delivered_lba, int setloc_lba)
{
    s_source_clock = 0;
    stat_reg = CDSTAT_MOTOR;
    s_source_seek_paused = 0;
    mode_reg = 0;
    lba_to_msf(cursor_lba, 150, &read_min, &read_sec, &read_sect);
    last_sector_lba = last_delivered_lba;
    s_setloc_lba = setloc_lba;
    setloc_pending = 1;
}

static int charge_from(int origin, int target)
{
    return apply_speed(source_seek_lower_bound(origin, target, 1, 0, 0));
}

int main(void)
{
    /* (a) SeekL to X, Setloc X, ReadN: zero travel. */
    drive(263062, -1, 263062);
    check(implicit_read_seek_cycles() == charge_from(263062, 263062), "(a) repeated target after SeekL has zero travel");
    check(charge_from(263062, 263062) < charge_from(0, 263062), "(a) zero travel is cheaper than the full seek");

    /* (b) Data delivered at A, SeekL to B, Setloc B, ReadN: travel 0, not |B-A|. */
    drive(263062, 1000, 263062);
    check(implicit_read_seek_cycles() == charge_from(263062, 263062), "(b) seek target, not last delivered sector, is the origin");
    check(implicit_read_seek_cycles() != charge_from(1000, 263062), "(b) travel from the last delivered sector is not charged");

    /* (c) Data delivered at A, SeekL to B, Setloc C, ReadN: travel |C-B|. */
    drive(263062, 1000, 270000);
    check(implicit_read_seek_cycles() == charge_from(263062, 270000), "(c) travel measured from the seek target");

    /* S4: after reset the position is LBA 0; the origin never goes negative. */
    drive(0, -1, 5000);
    check(implicit_read_seek_cycles() == charge_from(0, 5000), "S4 reset position is LBA 0");
    drive(-10, -1, 5000);
    check(implicit_read_seek_cycles() == charge_from(0, 5000), "S4 a negative position clamps to 0");

    /* No Setloc pending: no implicit seek. */
    drive(263062, 1000, 263062);
    setloc_pending = 0;
    check(implicit_read_seek_cycles() == 0, "no pending Setloc, no implicit seek");

    printf("cdrom implicit seek origin: %u checks passed\n", checks);
    return 0;
}
