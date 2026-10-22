/* PS1B-109: GetlocP while a read on the source clock is still seeking.
 *
 * Spyro the Dragon (USA) 5523M sends SetMode C8h, SetFilter, Setloc 25:14:48
 * (sector 113448) and ReadS, then GetlocP once every two returns while the
 * status stays 42h, motor and seek (PSX-SPX "Status code (stat)"). On the
 * source clock the first sector is due at cycle 939,560,040; the sector period
 * at double speed is 225,792 cycles, 150 sectors per second (PSX-SPX "Setmode
 * - Command 0Eh" bit 7; "CDROM Incoming Data / Buffer Overrun Timings"). The
 * admitted source run names the target up to return 1659 and the target + 1 at
 * return 1661, 132,712 cycles before the first delivery. The reply is track,
 * index, relative and absolute mm:ss:sect in BCD (PSX-SPX "GetlocP - Command
 * 11h").
 *
 * Rows:
 *   F1  ReadS on the source clock seeks; the first delivery is due at
 *       939,560,040, one period 225,792.
 *   F2  Return 1659, 1,264,488 cycles before the delivery: the target.
 *   F3  One cycle earlier than one period before the delivery: the target.
 *   F4  Exactly one period before the delivery: the target + 1 (the boundary
 *       this implementation chooses; it was not measured).
 *   F5  Return 1661, 132,712 cycles before the delivery: the target + 1 in
 *       both fields, 01 01 25 12 49 25 14 49. The status stays 42h, nothing is
 *       delivered, and the read timer is unchanged.
 *   F6  One cycle before the delivery: the target + 1.
 *   F7  On the delivery cycle the read is established and G2b answers the
 *       delivery cursor + 1, 113450, as before this change.
 *   F8  The default path (no source clock), the same seek and the same lead:
 *       the delivery cursor, as before this change.
 *   F9  The source read pipeline without the source clock tape: the same.
 * Controller-level. Synthetic sectors only; no BIOS or game data.
 *
 * usage: cdrom_getlocp_first_delivery_test <random tape>
 * tape: runtime/tests/cd_source_cold_random_256.psxrng */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

#define TARGET 113448
#define DUE 939560040ull
#define PERIOD_2X 225792

static const char *tape_path;

/* Every draw from an all-zero tape is 0 and takes one word. */
static void zero_tape(unsigned words) {
    free(s_source_clock_tape.bytes);
    s_source_clock_tape.bytes = calloc(words, 4);
    s_source_clock_tape.count = words; s_source_clock_tape.cursor = 0;
}
static void run(uint8_t cmd) { irq_flag = 0; s_source_ready_due = 0; response_clear(); exec_command(cmd); }
static void profile(int source_clock, int pipeline) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", source_clock ? "octoshock-2.2.2" : "");
    set_model("PSX_CD_TOC_SEEK_MODEL", source_clock ? "octoshock-2.2.2" : "");
    set_model("PSX_CD_READ_START_MODEL", pipeline ? "octoshock-2.2.2-pipeline" : "");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE", source_clock ? tape_path : "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
    if (source_clock) zero_tape(64);
}
/* The game's sequence: SetMode C8h, Setloc 25:14:48, ReadS at cycle `at`, from
 * a spinning drive whose cursor stands 2,000 sectors before the target.
 * Returns the read timer, the time to the first delivery. */
static int start_read(uint64_t at) {
    stat_reg = CDSTAT_MOTOR; s_source_seek_paused = 0;
    lba_to_msf(TARGET - 2000, 150, &read_min, &read_sec, &read_sect);
    last_sector_lba = TARGET - 2001;
    param_fifo[0] = 0xC8; param_count = 1; run(0x0E);
    int n = TARGET + 150;
    param_fifo[0] = bin_to_bcd(n / 4500); param_fifo[1] = bin_to_bcd(n / 75 % 60);
    param_fifo[2] = bin_to_bcd(n % 75); param_count = 3; run(0x02);
    psx_cycle_count = at;
    run(0x1B);
    irq_flag = 0;
    return read_delay;
}
/* Move the guest clock to `cycle` through the controller's own advance. */
static void advance_to(uint64_t cycle) { advance((int)(cycle - psx_cycle_count)); }
/* GetlocP executed at the current cycle; the absolute sector, or -100000. */
static int getlocp(uint8_t reply[8]) {
    run(0x11);
    if (irq_flag != CDIRQ_ACK || response_count != 8) return -100000;
    for (int i = 0; i < 8; i++) reply[i] = response_fifo[i];
    irq_flag = 0;
    int relative = (bcd_to_bin(reply[2]) * 60 + bcd_to_bin(reply[3])) * 75 + bcd_to_bin(reply[4]);
    int absolute = (bcd_to_bin(reply[5]) * 60 + bcd_to_bin(reply[6])) * 75 + bcd_to_bin(reply[7]) - 150;
    return relative == absolute ? absolute : -200000;
}

static void source_clock_rows(void) {
    uint8_t q[8];
    profile(1, 1);
    CHECK(s_source_clock && s_source_read_pipeline && !s_nymashock_drive, "the source clock runs on the read pipeline");
    int lead = start_read(0);
    profile(1, 1);
    CHECK(start_read(DUE - (uint64_t)lead) == lead, "the read start is the same at another cycle");
    CHECK(reading && stat_reg == (CDSTAT_MOTOR | CDSTAT_SEEK) && psx_cycle_count + (uint64_t)read_delay == DUE &&
          s_cd_timing_next_due == DUE && sector_delay_cycles() == PERIOD_2X,
          "F1: ReadS seeks, the first delivery is due at 939,560,040, period 225,792");

    advance_to(938295552ull);
    CHECK(read_delay == 1264488, "F2: the read timer is 1,264,488 at return 1659");
    CHECK(getlocp(q) == TARGET, "F2: return 1659 names the target 113448");
    advance_to(DUE - PERIOD_2X - 1);
    CHECK(getlocp(q) == TARGET, "F3: one cycle earlier than one period before the delivery names 113448");
    advance_to(DUE - PERIOD_2X);
    CHECK(getlocp(q) == TARGET + 1, "F4: exactly one period before the delivery names 113449");

    advance_to(939427328ull);
    unsigned delivered = s_dataready_fires;
    int timer = read_delay;
    const uint8_t return_1661[8] = {0x01, 0x01, 0x25, 0x12, 0x49, 0x25, 0x14, 0x49};
    CHECK(timer == 132712, "F5: the read timer is 132,712 at return 1661");
    CHECK(getlocp(q) == TARGET + 1 && !memcmp(q, return_1661, 8),
          "F5: return 1661 names 113449 in both fields: 01 01 25 12 49 25 14 49");
    CHECK(stat_reg == (CDSTAT_MOTOR | CDSTAT_SEEK) && reading && read_delay == timer &&
          s_dataready_fires == delivered && reads == 0,
          "F5: the status stays 42h and the read is not moved");
    advance_to(DUE - 1);
    CHECK(getlocp(q) == TARGET + 1 && (stat_reg & CDSTAT_SEEK), "F6: one cycle before the delivery names 113449");

    advance_to(DUE);
    CHECK(!(stat_reg & CDSTAT_SEEK) && (stat_reg & CDSTAT_READ) && reads == 1 &&
          msf_to_lba(read_min, read_sec, read_sect) == TARGET + 1,
          "F7: the first sector is delivered on its cycle");
    CHECK(getlocp(q) == TARGET + 2, "F7: G2b names the delivery cursor + 1, 113450");
    cd_tape_free(&s_source_clock_tape); s_source_clock = 0;
}

/* The same seek state without the source clock: the window rule is off. */
static void no_source_clock_rows(int pipeline, const char *what) {
    uint8_t q[8];
    char text[160];
    profile(0, pipeline);
    CHECK(!s_source_clock && s_source_read_pipeline == pipeline, what);
    int lead = start_read(0);
    CHECK(reading && (stat_reg & CDSTAT_SEEK) && lead > PERIOD_2X, "the read seeks for more than one period");
    advance_to((uint64_t)lead - 132712);
    snprintf(text, sizeof text, "%s: 132,712 cycles before the first delivery names the target", what);
    CHECK((stat_reg & CDSTAT_SEEK) && getlocp(q) == TARGET, text);
    advance_to((uint64_t)lead - 1);
    snprintf(text, sizeof text, "%s: one cycle before the first delivery names the target", what);
    CHECK((stat_reg & CDSTAT_SEEK) && getlocp(q) == TARGET, text);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <random tape>\n", argv[0]); return 64; }
    tape_path = argv[1];
    source_clock_rows();
    no_source_clock_rows(0, "F8: default path");
    no_source_clock_rows(1, "F9: source read pipeline without the source clock tape");
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: GetlocP names the target + 1 within one period of a source clock read's first delivery");
    return 0;
}
