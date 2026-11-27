/* PS1B-584: a command written while SeekL or SeekP is pending must not cancel
 * the seek itself.
 *
 * The default path cleared the pending second response for every new command
 * and the seek's state change rode on that response. A GetlocP or GetStat
 * sent during the seek therefore left the status at 42h (motor and seek) for
 * good, and GetlocP kept naming the sector before the seek. In Beetle
 * (mednafen/psx/cdc.cpp) a command write resets only the command state: the
 * drive keeps its own state, reaches the seek target and drops the seek bit
 * there; only the INT2 of the cancelled command is lost.
 *
 * Rows, for SeekL and SeekP:
 *   A1  GetlocP during the seek: no INT2 follows, the seek bit clears by the
 *       seek's own deadline, and GetlocP then names the seek target.
 *   A2  GetStat during the seek reports the seek bit, and a later GetStat
 *       reports the drive stopped at the target.
 *   A3  A Setloc during the seek stays pending after the drive arrives.
 *   A4  A ReadN during the seek replaces the arrival: the read goes on.
 *   A5  A savestate taken between the cancel and the arrival keeps the
 *       arrival, and a corrupted arrival section is refused.
 *   A6  An uncancelled seek still ends with INT2, as before.
 * Controller-level. Synthetic sectors only; no BIOS or game data.
 *
 * usage: cdrom_seek_cancel_arrival_test */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

static void default_path(int origin) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
    last_sector_lba = origin;
    lba_to_msf(origin, 150, &read_min, &read_sec, &read_sect);
    mode_reg = 0x80; setloc_pending = 0;
}

static int getlocp(void) {
    uint8_t reply[8];
    ack(); command(0x11);
    if (irq_flag != CDIRQ_ACK || response_count - response_read != 8) { ack(); return -1; }
    for (int i = 0; i < 8; i++) reply[i] = (uint8_t)cdrom_read(0x1f801801);
    ack();
    return msf_to_lba(bcd_to_bin(reply[5]), bcd_to_bin(reply[6]), bcd_to_bin(reply[7]));
}
static uint8_t getstat(void) {
    ack(); command(0x01);
    uint8_t s = irq_flag == CDIRQ_ACK && response_count - response_read == 1
                ? (uint8_t)cdrom_read(0x1f801801) : 0xFF;
    ack();
    return s;
}
/* Start a seek to 1000 from 100 and take its first response. Returns the
 * cycle its second response was due. */
static uint64_t start_seek(uint8_t seek) {
    default_path(100);
    target(1000);
    ack(); command(seek);
    CHECK(irq_flag == CDIRQ_ACK && pending.pending && pending.cmd == seek, "the seek is acknowledged");
    ack();
    return pending.due_cyc;
}
/* Run the drive to just past due and watch for any INT2. */
static int run_to(uint64_t due) {
    int int2 = 0;
    while (psx_cycle_count < due + 2000) {
        advance(500);
        if (irq_flag == CDIRQ_COMPLETE) { int2 = 1; ack(); }
    }
    return int2;
}

static void cancelled_seek(uint8_t seek, const char *name) {
    static uint8_t wire[1u << 20], bad[1u << 20];
    char what[160];
#define ROW(text) (snprintf(what, sizeof what, "%s (%s)", text, name), what)

    /* A1 */
    uint64_t due = start_seek(seek);
    CHECK(getlocp() == 100, ROW("A1: GetlocP during the seek names the old sector"));
    CHECK(!pending.pending, ROW("A1: the GetlocP ends the seek's second response"));
    CHECK(!run_to(due), ROW("A1: no INT2 follows the cancelled seek"));
    CHECK(!(stat_reg & CDSTAT_SEEK), ROW("A1: the seek bit clears by the seek's deadline"));
    CHECK(getlocp() == 1000, ROW("A1: GetlocP then names the seek target"));

    /* A2 */
    due = start_seek(seek);
    CHECK(getstat() == (CDSTAT_MOTOR | CDSTAT_SEEK), ROW("A2: GetStat during the seek reports 42h"));
    CHECK(!run_to(due), ROW("A2: no INT2 follows the cancelled seek"));
    CHECK(getstat() == CDSTAT_MOTOR, ROW("A2: GetStat after the deadline reports 02h"));
    CHECK(getlocp() == 1000, ROW("A2: and GetlocP names the seek target"));

    /* A3 */
    due = start_seek(seek);
    target(2000);
    CHECK(setloc_pending, "the Setloc is pending");
    (void)run_to(due);
    CHECK(!(stat_reg & CDSTAT_SEEK), ROW("A3: the drive arrives after a Setloc"));
    CHECK(setloc_pending, ROW("A3: the Setloc sent during the seek stays pending"));

    /* A4 */
    due = start_seek(seek);
    target(1000);
    ack(); command(0x06); ack();
    for (int i = 0; i < 40000 && irq_flag != CDIRQ_DATA_READY; i++) advance(1000);
    CHECK(irq_flag == CDIRQ_DATA_READY && reading, ROW("A4: a ReadN during the seek reads"));
    while (psx_cycle_count < due + 2000) { ack(); advance(500); }
    CHECK(reading && (stat_reg & CDSTAT_READ) && last_sector_have_raw != GETLOCL_SEEK_DONE,
          ROW("A4: the old seek's deadline does not undo the read"));
    ack();

    /* A5 */
    due = start_seek(seek);
    CHECK(getstat() == (CDSTAT_MOTOR | CDSTAT_SEEK), "the seek is under way");
    uint32_t size = cdrom_snapshot_bytes();
    CHECK(size <= sizeof wire, "the snapshot fits");
    cdrom_snapshot_write(wire);
    const uint64_t at = psx_cycle_count;
    default_path(500);
    psx_cycle_count = at;
    CHECK(cdrom_snapshot_read(wire, size), ROW("A5: the snapshot restores"));
    CHECK(!run_to(due), ROW("A5: no INT2 after the restore"));
    CHECK(!(stat_reg & CDSTAT_SEEK), ROW("A5: the restored drive arrives"));
    CHECK(getlocp() == 1000, ROW("A5: and GetlocP names the seek target"));
    memcpy(bad, wire, size); bad[size - 1] = 0x09;         /* not a seek */
    CHECK(!cdrom_snapshot_validate(bad, size), ROW("A5: an arrival for another command is refused"));
    memcpy(bad, wire, size); bad[size - 5] = 0; bad[size - 4] = 0;
    bad[size - 3] = 0; bad[size - 2] = 0;                  /* owed with no deadline */
    CHECK(!cdrom_snapshot_validate(bad, size), ROW("A5: an owed arrival with no deadline is refused"));
    /* A CDF7 state (before this tier) loads and owes no arrival. */
    psx_cycle_count = at;
    CHECK(cdrom_snapshot_read(wire, size - 9) && !s_seek_arrive_cmd,
          ROW("A5: a CDF7 state loads with no arrival owed"));

    /* A6 */
    due = start_seek(seek);
    CHECK(run_to(due), ROW("A6: an uncancelled seek still ends with INT2"));
    CHECK(!(stat_reg & CDSTAT_SEEK) && getlocp() == 1000, ROW("A6: and stands at its target"));
#undef ROW
}

int main(void) {
    cancelled_seek(0x15, "SeekL");
    cancelled_seek(0x16, "SeekP");
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: a command during SeekL or SeekP cancels only its response, not the seek");
    return 0;
}
