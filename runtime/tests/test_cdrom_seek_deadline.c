/* PS1G-103 (corpus PSX-CD-003): on the default (release) path a ReadN or ReadS
 * that consumes a pending Setloc does not deliver its first sector before the
 * first-sector wait plus four sector periods. That is three single-speed
 * sector periods at double speed and six at single speed.
 *
 * The case is Duke Nukem: Land of the Babes. With Start pressed during the
 * cutscene of the first level, the next read put the player out of bounds in
 * a mostly black scene when its first sector came one or two periods after
 * the command. With three periods the level loaded. Ape Escape's attract mode
 * kept its textures with one period and with three.
 *
 * The deadline is a floor. Where the seek model already waits longer (a
 * paused drive, a far target, a stopped motor) its deadline stands.
 *
 * Rows:
 *   D1 Drive in standby after a completed SeekL, new Setloc near, ReadN: the
 *      first sector comes at three periods, not before.
 *   D2 The ACK carries the status from before the command. The drive shows
 *      SEEK alone until the first sector and READ alone from it.
 *   D3 The sectors after the first keep the steady double-speed cadence.
 *   D4 A read that moves a running stream to another near target: the same.
 *   D5 ReadS as ReadN. At single speed the floor is six periods.
 *   D6 The floor follows the disc speed setting as the first-sector wait does.
 *   K1 After a Pause the seek model's later deadline is unchanged.
 *   K2 For a far target it is unchanged.
 *   K3 A read with no pending Setloc is unchanged: one period, READ at once.
 *   K4 A read that continues the running stream is not restarted.
 *   K5 A source (comparison) profile keeps its own, earlier deadline.
 *   S1 A state saved between Setloc and the read keeps the pending Setloc:
 *      the restored drive times the read as the original did.
 *   S2 The same with a stream running: the restored read moves to the Setloc
 *      target instead of continuing the old stream.
 *   S3 A state written without a pending Setloc restores without one, and
 *      the far flag is restored on its own. The size does not change.
 *   S4 A source profile's state bytes do not change: its pending Setloc stays
 *      in its own field.
 *   R1 The run report counters (psx_last_run_report.json "cdrom_seek_floor")
 *      count each read the floor held and the cycles it added, and no other
 *      read.
 * Controller-level, through the MMIO interface. Synthetic sectors only; no
 * BIOS or game data.
 *
 * usage: cdrom_seek_deadline_test <random tape> */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

#define PERIOD 451584   /* one single-speed sector: 33,868,800 / 75 cycles */
#define DRIVE_BITS (CDSTAT_SEEK | CDSTAT_READ | CDSTAT_PLAY)

static const char *tape_path;

static void default_path(uint8_t mode) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
    mode_reg = mode;
}
/* SeekL to `lba`, run to completion: the drive rests there in standby. */
static void standby_at(int lba) {
    target(lba); command(0x15); finish();
    CHECK(!reading && !s_source_seek_paused && !(stat_reg & DRIVE_BITS) && !setloc_pending,
          "a completed SeekL leaves the drive in standby with no Setloc pending");
}
static int cursor(void) { return msf_to_lba(read_min, read_sec, read_sect); }
static uint32_t held_reads(void) { uint32_t n = 0; cdrom_seek_floor_stats(&n, NULL); return n; }
static uint64_t held_cycles(void) { uint64_t c = 0; cdrom_seek_floor_stats(NULL, &c); return c; }

/* The read is issued; check its deadline, the status hand-over and the first
 * sector. `pre_status` is the status the ACK must carry. */
static void expect_first_sector(const char *name, int want, int lba, uint8_t pre_status) {
    char what[200];
    snprintf(what, sizeof what, "%s: first sector due after %d cycles (got %d)", name, want, read_delay);
    CHECK(read_delay == want, what);
    snprintf(what, sizeof what, "%s: the ACK carries the status from before the command", name);
    CHECK(response_fifo[response_read] == pre_status, what);
    snprintf(what, sizeof what, "%s: the drive shows SEEK alone before the first sector", name);
    CHECK((stat_reg & DRIVE_BITS) == CDSTAT_SEEK, what);
    int before = reads;
    ack(); advance(want - 1);
    snprintf(what, sizeof what, "%s: no sector before the deadline", name);
    CHECK(reads == before && (stat_reg & DRIVE_BITS) == CDSTAT_SEEK, what);
    advance(1);
    snprintf(what, sizeof what, "%s: the target sector arrives at the deadline", name);
    CHECK(reads == before + 1 && last_sector_lba == lba && irq_flag == CDIRQ_DATA_READY, what);
    snprintf(what, sizeof what, "%s: the first sector changes SEEK to READ", name);
    CHECK((stat_reg & DRIVE_BITS) == CDSTAT_READ, what);
    ack();
}

static void floor_rows(uint8_t cmd, const char *name) {
    char what[160];
    /* D1, D2: standby after SeekL, Setloc 100 sectors on, read. */
    default_path(0x80);
    standby_at(1000);
    const uint32_t reads0 = held_reads();
    const uint64_t cycles0 = held_cycles();
    target(1100); command(cmd);
    /* R1: the seek model asked for 20,000 cycles of travel and one period. */
    snprintf(what, sizeof what, "R1 %s: the held read and its added cycles are counted", name);
    CHECK(held_reads() == reads0 + 1 && held_cycles() == cycles0 + (uint64_t)(2 * PERIOD - 20000), what);
    snprintf(what, sizeof what, "D1 %s from standby", name);
    expect_first_sector(what, 3 * PERIOD, 1100, CDSTAT_MOTOR);
    /* D3: steady cadence afterwards. */
    int before = reads;
    advance(PERIOD / 2 - 1);
    CHECK(reads == before, "D3: the second sector is not early");
    advance(1);
    CHECK(reads == before + 1 && last_sector_lba == 1101, "D3: the second sector comes one double-speed period later");
    ack();
    /* D4: the stream runs; move it 400 sectors on. */
    target(1500); command(cmd);
    snprintf(what, sizeof what, "D4 %s from a running stream", name);
    expect_first_sector(what, 3 * PERIOD, 1500, CDSTAT_MOTOR | CDSTAT_READ);
    CHECK(held_reads() == reads0 + 2, "R1: the moved stream is the second held read");

    /* D5: single speed. */
    default_path(0x00);
    standby_at(1000);
    target(1100); command(cmd);
    snprintf(what, sizeof what, "D5 %s at single speed", name);
    expect_first_sector(what, 6 * PERIOD, 1100, CDSTAT_MOTOR);
}

static void speed_setting(void) {
    /* D6: disc speed 2x halves the first-sector wait and the sector period;
     * the floor is built from both. */
    default_path(0x80);
    g_disc_speed_divisor = 2;
    standby_at(1000);
    target(1100); command(0x06);
    CHECK(read_delay == 3 * PERIOD / 2, "D6: the floor halves with the disc speed setting");
    CHECK((stat_reg & DRIVE_BITS) == CDSTAT_SEEK, "D6: still a seek until the first sector");
}

static void later_deadlines_stand(void) {
    /* K1: read, Pause, Setloc 10 sectors on, read. The paused restart is
     * 20,000 + 2,475,904 / 2 cycles; then the one-period first-sector wait. */
    default_path(0x80);
    standby_at(1000);
    target(1000); command(0x06); ack(); advance(read_delay); ack();
    command(0x09); finish();
    CHECK(!reading && s_source_seek_paused && !(stat_reg & DRIVE_BITS), "K1: the drive is paused");
    const int from = cursor();
    uint32_t held = held_reads();
    target(from + 10); command(0x06);
    CHECK(read_delay == 20000 + 1237952 + PERIOD, "K1: after a Pause the seek model's deadline is unchanged");
    CHECK(read_delay > 3 * PERIOD, "K1: and it is later than the floor");
    CHECK(held_reads() == held, "R1: a read after a Pause is not counted");

    /* K2: 9,000 sectors away. Travel 9,000 * 1568 / 15, the long-seek settle
     * 10,160,640, then the first-sector wait. */
    default_path(0x80);
    standby_at(1000);
    target(10000); command(0x06);
    CHECK(read_delay == 940800 + 10160640 + PERIOD, "K2: a far target keeps the seek model's deadline");
    CHECK(held_reads() == held, "R1: a read to a far target is not counted");

    /* K3: a completed SeekL consumed the Setloc; the read that follows has
     * no implicit seek. */
    default_path(0x80);
    standby_at(1000);
    command(0x06);
    CHECK(held_reads() == held, "R1: a read with no pending Setloc is not counted");
    CHECK(read_delay == PERIOD, "K3: a read with no pending Setloc waits one period");
    CHECK((stat_reg & DRIVE_BITS) == CDSTAT_READ, "K3: and reads at once, with no SEEK");
    int before = reads;
    ack(); advance(PERIOD);
    CHECK(reads == before + 1 && last_sector_lba == 1000, "K3: it delivers the seek target");
    ack();

    /* K4: the stream runs and the game names the sector it would deliver
     * next. The read continues; nothing is re-armed. */
    advance(1000);
    const int next = cursor(), remaining = read_delay;
    target(next); command(0x06);
    CHECK(reading && read_delay == remaining && (stat_reg & DRIVE_BITS) == CDSTAT_READ,
          "K4: a read that continues the running stream is not restarted");
    CHECK(held_reads() == held, "R1: a continued stream is not counted");
}

/* K5: a source profile times the same read by its own rules: three sector
 * periods at the current speed for the pipeline, the 20,000-cycle minimum
 * travel, and a draw below 25,000. */
static void source_profile_unchanged(const char *drive) {
    char what[160];
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", "octoshock-2.2.2"); set_model("PSX_CD_TOC_SEEK_MODEL", "octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL", "octoshock-2.2.2-pipeline");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE", tape_path); set_model("PSX_CD_DRIVE_MODEL", drive);
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
    CHECK(s_source_clock, "K5: the profile runs on the source clock");
    mode_reg = 0x80;
    stat_reg = CDSTAT_MOTOR; reading = 0; s_source_seek_paused = 0; source_drive_head_valid = 0;
    lba_to_msf(1000, 150, &read_min, &read_sec, &read_sect);
    param_fifo[0] = bin_to_bcd((1100 + 150) / 4500);
    param_fifo[1] = bin_to_bcd((1100 + 150) / 75 % 60);
    param_fifo[2] = bin_to_bcd((1100 + 150) % 75);
    param_count = 3; irq_flag = 0; response_clear();
    exec_command(0x02);
    irq_flag = 0; response_clear();
    const uint32_t held = held_reads();
    exec_command(0x06);
    snprintf(what, sizeof what, "K5 %s: the source profile keeps its own deadline (%d cycles)",
             *drive ? drive : "no drive model", read_delay);
    CHECK(reading && read_delay >= 3 * (PERIOD / 2) + 20000 && read_delay < 3 * (PERIOD / 2) + 20000 + 25000, what);
    CHECK(held_reads() == held, "R1: a source profile's read is not counted");

    /* S4: the pending Setloc of a source profile lives in the profile's own
     * field. Two states that differ only in it differ in one byte. */
    uint32_t size = cdrom_snapshot_bytes();
    uint8_t *with = malloc(size), *without = malloc(size);
    stop_read_stream();
    setloc_seek_far = 0;
    setloc_pending = 1; cdrom_snapshot_write(with);
    setloc_pending = 0; cdrom_snapshot_write(without);
    uint32_t differ = 0;
    for (uint32_t i = 0; i < size; ++i) differ += with[i] != without[i];
    snprintf(what, sizeof what, "S4 %s: a pending Setloc changes one byte of a source profile's state (%u changed)",
             *drive ? drive : "no drive model", differ);
    CHECK(differ == 1, what);
    free(with); free(without);
}

static void state_rows(void) {
    /* S1: save between Setloc and ReadN, from standby. */
    default_path(0x80);
    standby_at(1000);
    target(1100);
    uint32_t size = cdrom_snapshot_bytes();
    uint8_t *snap = malloc(size);
    cdrom_snapshot_write(snap);
    command(0x06);
    const int original = read_delay;
    ack(); advance(read_delay); ack();
    command(0x09); finish();                 /* the abandoned timeline moves on */
    CHECK(cdrom_snapshot_read(snap, size), "S1: the state restores");
    CHECK(setloc_pending, "S1: the restored drive still has its Setloc pending");
    CHECK(cdrom_snapshot_bytes() == size, "S1: a pending Setloc does not change the size of the state");
    int before = reads;
    command(0x06);
    CHECK(read_delay == original, "S1: the restored read gets the deadline the original got");
    CHECK((stat_reg & DRIVE_BITS) == CDSTAT_SEEK, "S1: and seeks, as the original did");
    ack(); advance(read_delay);
    CHECK(reads == before + 1 && last_sector_lba == 1100, "S1: and delivers the Setloc target");
    ack();

    /* S2: a stream runs at 1100; Setloc to 5000; save; restore; ReadN. */
    target(5000);
    cdrom_snapshot_write(snap);
    advance(1000);
    CHECK(cdrom_snapshot_read(snap, size), "S2: the state restores");
    before = reads;
    command(0x06);
    CHECK((stat_reg & DRIVE_BITS) == CDSTAT_SEEK, "S2: the restored read leaves the old stream and seeks");
    ack(); advance(read_delay);
    CHECK(reads == before + 1 && last_sector_lba == 5000, "S2: the restored read delivers the Setloc target");
    ack();
    free(snap);

    /* S3: the far flag and the pending flag are restored independently. */
    default_path(0x80);
    size = cdrom_snapshot_bytes();
    snap = malloc(size);
    setloc_seek_far = 1; setloc_pending = 0;
    cdrom_snapshot_write(snap);
    setloc_seek_far = 0; setloc_pending = 1;
    CHECK(cdrom_snapshot_read(snap, size) && setloc_seek_far == 1 && !setloc_pending,
          "S3: a state without a pending Setloc restores without one, far flag kept");
    setloc_seek_far = 0; setloc_pending = 1;
    cdrom_snapshot_write(snap);
    setloc_seek_far = 1; setloc_pending = 0;
    CHECK(cdrom_snapshot_read(snap, size) && setloc_seek_far == 0 && setloc_pending,
          "S3: a pending Setloc restores without setting the far flag");
    free(snap);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <random tape>\n", argv[0]); return 64; }
    tape_path = argv[1];
    floor_rows(0x06, "ReadN");
    floor_rows(0x1B, "ReadS");
    speed_setting();
    later_deadlines_stand();
    state_rows();
    source_profile_unchanged("");
    source_profile_unchanged("nymashock-1.29.0");
    source_profile_unchanged("octoshock-2.7");
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: a read after Setloc waits at least three single-speed sector periods");
    return 0;
}
