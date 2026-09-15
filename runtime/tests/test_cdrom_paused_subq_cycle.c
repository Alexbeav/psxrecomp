/* Controller-level regression: GetlocP on a stopped Nymashock drive.
 *
 * GetlocP returns SubQBuf_Safe, the sub-Q of the last sector the drive
 * decoded -- not the last sector handed to the guest. The source keeps
 * decoding while the drive is stopped: its Update() calls HandlePlayRead for
 * DS_PAUSED and DS_STANDBY as well as DS_READING, and the tail of
 * HandlePlayRead cycles the cursor
 *
 *     CurSector++;
 *     if(DriveStatus == DS_PAUSED || DriveStatus == DS_STANDBY)
 *       if(CurSector >= (SeekTarget + 2)) CurSector = max(-150, CurSector - 9);
 *
 * so a paused drive walks a nine-sector window ending two above the position
 * Pause stopped at, decoding one sub-Q per sector period, forever. Reporting
 * the last delivered sector is correct only for the first tick after a Pause.
 *
 * Pinned to measured evidence. Mega Man X5 (USA) Training X #6377M snapshots
 * a GetlocP reply into 0x800ECDC0; at return 7311, five frames after the
 * Pause at 7306, the admitted source replies
 *
 *     01 01 41 09 68 41 11 68   = track 1, index 1, absolute sector 185243
 *
 * while the drive had last delivered sector 185251 and Pause had stopped the
 * head at 185250. 185243 is the third distinct position in the cycle, and the
 * elapsed 2,931,664 cycles are 12.98 sector periods at 2x -- twelve completed
 * ticks, which the table below lands on exactly.
 *
 * Synthetic positions only; no BIOS or game data.
 */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
void psx_irq_raise(uint32_t a, uint32_t b) { (void)b; i_stat |= 1u << a; }
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
    (void)p; memset(b, (int)(lba & 255u), (size_t)n); return 1;
}
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba; (void)b; (void)n; return 0;
}
int iso_read_subq(void *p, uint32_t lba, uint8_t *b, int n, int *v) {
    (void)p; (void)lba; (void)b; (void)n; *v = 0; return 0;
}
int iso_has_subq_replacements(void *p) { (void)p; return 0; }
uint32_t iso_sector_count(void *p) { (void)p; return 333000; }
int iso_track_count(void *p) { (void)p; return 1; }
uint32_t iso_track_start_lba(void *p, int t) { (void)p; (void)t; return 0; }
uint32_t iso_track_pregap_lba(void *p, int t) { (void)p; (void)t; return 0; }
int iso_track_is_audio(void *p, int t) { (void)p; (void)t; return 0; }

/* The route runs the drive at 2x (mode 0xC8), so one decode per half period. */
#define SECTOR_PERIOD (CDROM_SINGLE_SPEED_SECTOR_CYCLES / 2)

static int checks;

/* Issue GetlocP and return the absolute sector it reports, or a negative
 * code if the reply is not a well-formed eight-byte position. */
static int getlocp_sector(void) {
    uint8_t r[8];
    int i;
    cdrom_write(0x1f801800, 0);
    cdrom_write(0x1f801801, 0x11);
    if (irq_flag != CDIRQ_ACK) return -1;
    if (response_count - response_read != 8) return -2;
    for (i = 0; i < 8; ++i) r[i] = (uint8_t)cdrom_read(0x1f801801);
    cdrom_write(0x1f801800, 1);
    cdrom_write(0x1f801803, 0x1f);
    if (r[0] != 0x01 || r[1] != 0x01) return -3;          /* track 1, index 1 */
    {
        int rel = (bcd_to_bin(r[2]) * 60 + bcd_to_bin(r[3])) * 75 + bcd_to_bin(r[4]);
        int abs_ = (bcd_to_bin(r[5]) * 60 + bcd_to_bin(r[6])) * 75 + bcd_to_bin(r[7]);
        if (abs_ - rel != 150) return -4;                 /* track 1 pregap */
        return abs_ - 150;
    }
}

/* Put the controller in the state Pause leaves behind: stopped, head armed at
 * `target`, last delivery at `delivered`. */
static void stop_drive_at(int target, int delivered, uint64_t now) {
    reading = 0;
    cdda_playing = 0;
    stat_reg &= (uint8_t)~(CDSTAT_READ | CDSTAT_SEEK | CDSTAT_PLAY);
    mode_reg |= 0x80;                       /* 2x */
    source_drive_hold_logical = 1;          /* start_read_stream sets this */
    last_sector_lba = delivered;
    psx_cycle_count = now;
    source_drive_subq_lba = -1;
    source_drive_head_valid = 1;
    source_drive_head_lba = source_drive_head_target = target;
    source_drive_head_due = now + SECTOR_PERIOD;
}

int main(void) {
    /* Measured at Mega Man X5 return 7306/7311. */
    const int target = 185250;      /* where Pause stopped the head */
    const int delivered = 185251;   /* last sector handed to the guest */
    /* Ticks 1..20 of the paused cycle. Entry 12 is the reply the admitted
     * source produced at return 7311. */
    const int expect[20] = {
        185250, 185251, 185243, 185244, 185245, 185246, 185247, 185248,
        185249, 185250, 185251, 185243, 185244, 185245, 185246, 185247,
        185248, 185249, 185250, 185251
    };
    uint64_t base = 1000000;
    int i;

    cdrom_init("authored");
    s_nymashock_drive = 1;
    stop_drive_at(target, delivered, base);

    /* Before the first decode there is no sub-Q to report, so the last
     * delivered sector stands in. */
    if (getlocp_sector() != delivered) return 1;
    checks++;

    for (i = 1; i <= 20; ++i) {
        int got;
        psx_cycle_count = base + (uint64_t)i * SECTOR_PERIOD + SECTOR_PERIOD / 2;
        got = getlocp_sector();
        if (got != expect[i - 1]) {
            printf("tick %d: GetlocP reported %d, expected %d\n", i, got, expect[i - 1]);
            return 2;
        }
        checks++;
    }

    /* The cycle never leaves its nine-sector window, and only its first two
     * entries coincide with the last delivered sector -- which is why the
     * pre-fix reply was right at 7306 and wrong from 7307 on. */
    for (i = 0; i < 20; ++i) {
        if (expect[i] > target + 1 || expect[i] < target - 7) return 3;
        checks++;
    }

    /* An Octoshock-era drive keeps the old answer: the cycling head is a
     * Nymashock behaviour and Tekken 3 / Pepsiman must not move. */
    s_nymashock_drive = 0;
    stop_drive_at(target, delivered, base);
    s_nymashock_drive = 0;
    for (i = 1; i <= 20; ++i) {
        psx_cycle_count = base + (uint64_t)i * SECTOR_PERIOD + SECTOR_PERIOD / 2;
        if (getlocp_sector() != delivered) return 4;
        checks++;
    }

    /* A drive that never decoded a sub-Q still falls back cleanly. */
    s_nymashock_drive = 1;
    stop_drive_at(target, delivered, base);
    source_drive_head_valid = 0;
    psx_cycle_count = base + 100 * SECTOR_PERIOD;
    if (getlocp_sector() != delivered) return 5;
    checks++;

    printf("GetlocP paused-drive sub-Q cycle: %d checks\n", checks);
    return 0;
}
