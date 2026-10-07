/* PS1G-95: GetlocP after a completed SeekL or SeekP on the default (release)
 * path.
 *
 * The case is Mega Man Legends 2 (SLES-03556) after the BIOS logos. The game
 * sends Setloc and SeekL and then repeats GetlocP. The reply kept naming the
 * last sector the drive had delivered before the seek (sector 10,300) and not
 * the seek target, the game did not go on, and the screen stayed black (wave 4
 * record of 2026-09-13). The wave 4 correction made GetlocP name the drive
 * cursor whenever the drive is stopped. A later merge brought the old branch
 * back, and cdrom_paused_subq_cycle_test now pins the last delivered sector
 * after a Pause. So the seek and the Pause get different answers.
 *
 * Rows:
 *   P1  Stopped after a delivered sector: GetlocP names that sector.
 *   P2  Setloc alone does not move the reply.
 *   P3  After a completed SeekL or SeekP, GetlocP names the seek target.
 *   P4  GetlocL has no header there (INT5 80h), as before.
 *   P5  A later Setloc does not move the reply.
 *   P6  A Pause sent while the drive stands at the target keeps the reply.
 *   P7  A savestate round trip keeps the reply.
 *   P8  The position queries read no data sector.
 *   P9  A read from the target: GetlocP names the drive cursor, which is the
 *       sector after the delivered one. GetlocL names the delivered sector.
 *   P10 A Pause after delivered sectors: GetlocP names the last delivered
 *       sector, as cdrom_paused_subq_cycle_test pins it. A rule "always the
 *       drive cursor" answers one sector higher here.
 *   P11 A second seek from there names its own target when it has completed.
 *   P12 Play, ReadTOC and Init after the seek end the rule: the reply is the
 *       last delivered sector, as it was before this change.
 *   P13 CD audio keeps its own position.
 *   P14 A source (comparison) profile is unchanged. After a completed seek
 *       the snapshot byte is the plain no-header marker, and without a drive
 *       model GetlocP names the last delivered sector.
 * Controller-level. Synthetic sectors only; no BIOS or game data. Rows P1 to
 * P3, P5, P8, P9 and P13 come from the wave 4 test (test_cdrom_position.c).
 *
 * usage: cdrom_getlocp_after_seek_test <random tape>
 * tape: runtime/tests/cd_source_cold_random_256.psxrng */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

static const char *tape_path;

static void default_path(int origin) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
    /* The state a delivered sector leaves, seeded without a synthetic disc
     * read: the last delivered sector and the drive cursor. */
    last_sector_lba = origin;
    lba_to_msf(origin, 150, &read_min, &read_sec, &read_sect);
    mode_reg = 0x80; setloc_pending = 0;
}
static void source_profile(const char *drive) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", "octoshock-2.2.2"); set_model("PSX_CD_TOC_SEEK_MODEL", "octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL", "octoshock-2.2.2-pipeline");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE", tape_path); set_model("PSX_CD_DRIVE_MODEL", drive);
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
}

/* GetlocP (11h) or GetlocL (10h) through the registers. Returns the absolute
 * sector of an INT3 reply, or -1 for any other reply. */
static int position(uint8_t cmd) {
    uint8_t reply[8];
    int offset = cmd == 0x11 ? 5 : 0;
    ack(); command(cmd);
    if (irq_flag != CDIRQ_ACK || response_count - response_read != 8) { ack(); return -1; }
    for (int i = 0; i < 8; i++) reply[i] = (uint8_t)cdrom_read(0x1f801801);
    ack();
    return msf_to_lba(bcd_to_bin(reply[offset]), bcd_to_bin(reply[offset + 1]),
                      bcd_to_bin(reply[offset + 2]));
}
/* GetlocL answers INT5 with the error byte 80h. */
static int getlocl_has_no_header(void) {
    ack(); command(0x10);
    int none = irq_flag == CDIRQ_ERROR && response_count - response_read == 2 &&
               (response_fifo[response_read] & CDSTAT_ERROR) &&
               response_fifo[(response_read + 1) & 15] == 0x80;
    ack();
    return none;
}
static void wait_for(uint8_t irq) {
    for (int i = 0; i < 40000 && irq_flag != irq; i++) advance(1000);
}
/* Send a two-response command and take both responses. */
static void complete(uint8_t cmd) {
    ack(); command(cmd);
    CHECK(irq_flag == CDIRQ_ACK, "the command is acknowledged");
    ack(); wait_for(CDIRQ_COMPLETE);
    CHECK(irq_flag == CDIRQ_COMPLETE, "the command completes with INT2");
    ack();
}

static void seek_then_pause(uint8_t seek, const char *name) {
    static uint8_t wire[1u << 20];
    char what[160];
#define ROW(text) (snprintf(what, sizeof what, "%s (%s)", text, name), what)

    default_path(100);
    CHECK(!s_source_clock, "the default path has no source clock");
    CHECK(position(0x11) == 100, ROW("P1: stopped after a delivered sector, GetlocP names it"));
    target(1000);
    CHECK(position(0x11) == 100, ROW("P2: Setloc alone does not move the reply"));

    complete(seek);
    CHECK(!(stat_reg & (CDSTAT_SEEK | CDSTAT_READ | CDSTAT_PLAY)) && !reading, "the seek ends stopped");
    CHECK(position(0x11) == 1000, ROW("P3: GetlocP names the completed seek target"));
    CHECK(getlocl_has_no_header(), ROW("P4: GetlocL has no header after the seek"));
    target(2000);
    CHECK(position(0x11) == 1000, ROW("P5: a later Setloc does not move the reply"));

    complete(0x09);
    CHECK(position(0x11) == 1000, ROW("P6: a Pause at the target keeps the reply"));
    CHECK(getlocl_has_no_header(), ROW("P6: and GetlocL still has no header"));

    uint32_t size = cdrom_snapshot_bytes();
    CHECK(size <= sizeof wire, "the snapshot fits");
    cdrom_snapshot_write(wire);
    default_path(500);                                   /* another live state */
    CHECK(position(0x11) == 500, "the other state answers for itself");
    CHECK(cdrom_snapshot_read(wire, size), "the snapshot restores");
    CHECK(position(0x11) == 1000, ROW("P7: a savestate round trip keeps the reply"));
    CHECK(reads == 0, ROW("P8: the position queries read no data sector"));

    /* The read the game sends next. */
    target(1000);
    ack(); command(0x06);
    CHECK(irq_flag == CDIRQ_ACK, "ReadN is acknowledged");
    ack(); wait_for(CDIRQ_DATA_READY);
    CHECK(reading && last_sector_lba == 1000, "ReadN delivers the sector at the seek target");
    CHECK(position(0x11) == 1001, ROW("P9: an active read reports the drive cursor"));
    CHECK(position(0x10) == 1000, ROW("P9: GetlocL names the delivered sector"));

    complete(0x09);
    const int delivered = last_sector_lba;
    CHECK(!reading && delivered >= 1000, "Pause ends the read");
    CHECK(position(0x11) == delivered, ROW("P10: after a Pause GetlocP names the last delivered sector"));
    CHECK(position(0x10) == delivered, ROW("P10: and GetlocL keeps its header"));

    target(3000);
    complete(seek);
    CHECK(position(0x11) == 3000, ROW("P11: a second seek names its own target"));
    CHECK(getlocl_has_no_header(), ROW("P11: and drops the header"));

    /* P12: the drive leaves the target. */
    ack(); command(0x03); ack();
    CHECK(position(0x11) == delivered, ROW("P12: Play ends the rule"));

    default_path(100); target(1000); complete(seek);
    complete(0x1E);
    CHECK(position(0x11) == 100, ROW("P12: ReadTOC ends the rule"));

    default_path(100); target(1000); complete(seek);
    complete(0x0A);
    CHECK(position(0x11) == 100, ROW("P12: Init ends the rule"));
#undef ROW
}

/* P14. The commands run directly, as cdrom_read_starts_motor_test does for
 * its source rows: the register interface of the source clock has its own
 * reception delays, and they are not the subject here. */
static void source_profile_unchanged(const char *drive) {
    char what[160];
    source_profile(drive);
    CHECK(s_source_clock, "the profile runs on the source clock");
    mode_reg = 0x80;
    last_sector_lba = 100;
    lba_to_msf(100, 150, &read_min, &read_sec, &read_sect);
    param_fifo[0] = bin_to_bcd((1000 + 150) / 4500);
    param_fifo[1] = bin_to_bcd((1000 + 150) / 75 % 60);
    param_fifo[2] = bin_to_bcd((1000 + 150) % 75);
    param_count = 3; irq_flag = 0; response_clear();
    exec_command(0x02);                       /* Setloc */
    param_count = 0; irq_flag = 0; response_clear();
    exec_command(0x15);                       /* SeekL */
    irq_flag = 0; response_clear();
    for (int i = 0; i < 40000 && pending.pending; i++) advance(1000);
    snprintf(what, sizeof what, "P14 %s: the seek completes", *drive ? drive : "no drive model");
    CHECK(!pending.pending && irq_flag == CDIRQ_COMPLETE, what);
    snprintf(what, sizeof what, "P14 %s: the snapshot byte is the plain no-header marker (%02X)",
             *drive ? drive : "no drive model", last_sector_have_raw);
    CHECK(last_sector_have_raw == GETLOCL_NO_HEADER, what);
    if (!*drive) {
        irq_flag = 0; response_clear(); param_count = 0;
        exec_command(0x11);
        int reply = msf_to_lba(bcd_to_bin(response_fifo[(response_read + 5) & 15]),
                               bcd_to_bin(response_fifo[(response_read + 6) & 15]),
                               bcd_to_bin(response_fifo[(response_read + 7) & 15]));
        snprintf(what, sizeof what, "P14: without a drive model GetlocP names the last delivered sector (%d)", reply);
        CHECK(irq_flag == CDIRQ_ACK && reply == 100, what);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <random tape>\n", argv[0]); return 64; }
    tape_path = argv[1];
    seek_then_pause(0x15, "SeekL");
    seek_then_pause(0x16, "SeekP");

    default_path(100); cdda_playing = 1; cdda_track = 1; cdda_lba = 500;
    CHECK(position(0x11) == 500, "P13: CD audio keeps its own position");

    source_profile_unchanged("");
    source_profile_unchanged("nymashock-1.29.0");
    source_profile_unchanged("octoshock-2.7");

    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: GetlocP names a completed seek's target on the default path only");
    return 0;
}
