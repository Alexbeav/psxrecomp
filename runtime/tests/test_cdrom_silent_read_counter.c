/* PS1B-317 fleet check: the drive counts read streams that ended before
 * their first sector, and the longest run of them in a row. The run report
 * carries the three numbers (psx_last_run_report.json "cdrom"), so a title
 * whose reads never deliver is found from a release build's report.
 *
 * What is counted:
 *   - a stream "starts" at every ReadN/ReadS that begins a new stream;
 *   - it "ends" at a Pause, a Stop, or the next read start;
 *   - it is "silent" when no sector was read off the disc between the two;
 *   - a stream that is still open is not counted.
 * The run resets when a stream that read a sector ends.
 *
 * The counters are diagnostics: the last case shows the same command
 * sequence gives the same delays and status with or without earlier silent
 * reads on the books.
 *
 * Controller-level, through the MMIO interface. Synthetic sectors only; no
 * BIOS or game data. */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

static uint32_t starts, silent, longest;
static void stats(void) { cdrom_silent_read_stats(&starts, &silent, &longest); }
#define EXPECT(s, q, l, what) do { stats(); if (starts != (uint32_t)(s) || silent != (uint32_t)(q) || longest != (uint32_t)(l)) { \
    fprintf(stderr, "FAIL: %s: starts %u silent %u longest run %u, want %d %d %d\n", what, starts, silent, longest, (s), (q), (l)); failures++; } } while (0)

static void fresh(void) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
    mode_reg = 0x80;
}
/* Setloc + ReadN/ReadS; returns the delay to the first sector. */
static int read_at(uint8_t cmd, int lba) { target(lba); command(cmd); ack(); return read_delay; }
static void pause_drive(void) { command(0x09); finish(); }
static void stop_drive(void) { command(0x08); finish(); }
/* A read cancelled before its first sector, as a library retry does. */
static void silent_read(int lba) {
    int delay = read_at(0x06, lba);
    advance(delay / 2);
    pause_drive();
}
/* A read that delivers `sectors` sectors and is then paused. */
static void good_read(int lba, int sectors) {
    int before = reads;
    int delay = read_at(0x06, lba);
    advance(delay); ack();
    for (int i = 1; i < sectors; ++i) { advance(read_delay); ack(); }
    CHECK(reads >= before + sectors, "the good read delivers its sectors");
    pause_drive();
}

int main(void) {
    fresh();
    /* The counters belong to the process, not to the session: read them as
     * differences from here. */
    stats();
    const uint32_t s0 = starts, q0 = silent;
    CHECK(s0 == 0 && q0 == 0 && longest == 0, "a new process has counted nothing");

    good_read(1000, 3);
    EXPECT(1, 0, 0, "a read that delivers is not silent");

    /* Still open: started, not ended, not counted yet. */
    int delay = read_at(0x06, 5000);
    EXPECT(2, 0, 0, "an open read is not counted while it waits");
    advance(delay / 2);
    EXPECT(2, 0, 0, "nor half way to its first sector");
    pause_drive();
    EXPECT(2, 1, 1, "a Pause before the first sector ends a silent read");

    /* The retry pattern: the same read restarted and cancelled, five more. */
    for (int i = 0; i < 5; ++i) silent_read(5000);
    EXPECT(7, 6, 6, "six silent reads in a row");

    good_read(5000, 2);
    EXPECT(8, 6, 6, "a read that delivers keeps the record and ends the run");
    silent_read(9000);
    silent_read(9000);
    EXPECT(10, 8, 6, "a new run starts from one; the longest stays");

    /* Ended by the next read start, with no Pause between. */
    delay = read_at(0x06, 20000);
    advance(delay / 2);
    (void)read_at(0x06, 30000);
    EXPECT(12, 9, 6, "a read started over a silent one ends it");
    /* Ended by Stop. */
    stop_drive();
    EXPECT(12, 10, 6, "Stop ends a silent read");
    /* Commands with no stream open count nothing. */
    stop_drive(); pause_drive();
    EXPECT(12, 10, 6, "Stop and Pause with nothing open count nothing");

    /* ReadS counts as ReadN. */
    delay = read_at(0x1B, 40000);
    advance(delay / 2);
    pause_drive();
    EXPECT(13, 11, 6, "ReadS is a read stream too");

    /* The run since the last good read is now five. Nine more make it the
     * record. */
    for (int i = 0; i < 9; ++i) silent_read(40000);
    EXPECT(22, 20, 14, "fourteen in a row is the new record");

    /* Diagnostics only: the same sequence is timed the same on a drive that
     * has counted silent reads and on a fresh one. */
    int timed[2][3], status[2];
    for (int pass = 0; pass < 2; ++pass) {
        fresh();
        if (pass) for (int i = 0; i < 4; ++i) silent_read(7000);
        cdrom_init("synthetic"); psx_cycle_count = 0; mode_reg = 0x80;
        timed[pass][0] = read_at(0x06, 12000);
        advance(timed[pass][0]); ack();
        timed[pass][1] = read_delay;
        command(0x09);
        timed[pass][2] = (int)(pending.due_cyc - psx_cycle_count);
        finish();
        status[pass] = stat_reg;
    }
    CHECK(timed[0][0] == timed[1][0] && timed[0][1] == timed[1][1] && timed[0][2] == timed[1][2] &&
          status[0] == status[1], "the counters change no delay and no status");

    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: silent read streams are counted");
    return 0;
}
