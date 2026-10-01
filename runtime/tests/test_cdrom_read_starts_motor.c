/* PS1B-317, behaviour spec 6.11: on the default (release) path a ReadN or
 * ReadS start spins the motor up, and a Pause does not stop it.
 *
 * The case is Time Crisis after its logo. The game sends Stop until the motor
 * bit clears, then Setloc and ReadN. A read that starts with the motor off is
 * charged the spin-up (one second in cd_seek_delay.h) on top of the travel.
 * The game's CD library restarts a read that delivered nothing for 60 vblanks
 * (one second): Pause, Setloc, ReadN. When the motor bit stayed clear across
 * that Pause, every attempt was charged a new full second and no sector ever
 * arrived.
 *
 * Rows:
 *   M1 Stop still clears the motor bit.
 *   M2 The first read after a Stop pays the spin-up once: its delay is the
 *      seek model's motor-off charge plus the first-sector wait.
 *   M3 That read start sets the motor bit (PSX-SPX: Read starts the motor).
 *   M4 A Pause before the first sector leaves the motor bit set.
 *   M5 The read re-issued after that Pause is charged with the motor on and
 *      delivers its first sector inside the 60-vblank retry window.
 *   M6 ReadS behaves as ReadN.
 *   M7 A source (comparison) profile is unchanged: the read start leaves the
 *      motor bit as it found it, and the re-issued read pays the motor-off
 *      charge again.
 * Controller-level, through the MMIO interface. Synthetic sectors only; no
 * BIOS or game data.
 *
 * usage: cdrom_read_starts_motor_test <random tape> */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

#define NTSC_VBLANK_CYCLES 564480
#define RETRY_WINDOW       (60 * NTSC_VBLANK_CYCLES)   /* the library's 60 vblanks */
#define LOGO_END_LBA       4527     /* last sector the game read before Stop */
#define NEXT_FILE_LBA      13439    /* Setloc 03:01:14 */

static const char *tape_path;

static void default_path(void) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
}
static void source_profile(const char *drive) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", "octoshock-2.2.2"); set_model("PSX_CD_TOC_SEEK_MODEL", "octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL", "octoshock-2.2.2-pipeline");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE", tape_path); set_model("PSX_CD_DRIVE_MODEL", drive);
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
}
static void setmode(uint8_t mode) {
    cdrom_write(0x1f801800, 0); cdrom_write(0x1f801802, mode); command(0x0E); ack();
}
static void cursor_at(int lba) { lba_to_msf(lba, 150, &read_min, &read_sec, &read_sect); }
/* The game's loop: Stop until the status shows the motor off. */
static int stop_until_motor_off(void) {
    int sent = 0;
    while (sent < 8) {
        command(0x08); finish(); sent++;
        if (!(stat_reg & CDSTAT_MOTOR)) break;
    }
    return sent;
}
/* Setloc + ReadN/ReadS as the library sends them; returns the scheduled delay
 * to the first sector. */
static int start_read(uint8_t cmd, int lba) {
    target(lba);
    command(cmd); ack();
    return read_delay;
}

static void retry_after_stop(uint8_t cmd, const char *name) {
    char what[160];
    default_path();
    CHECK(!s_source_clock, "the default path has no source clock");
    setmode(0xA0);
    cursor_at(LOGO_END_LBA + 1); last_sector_lba = LOGO_END_LBA;
    CHECK(stat_reg & CDSTAT_MOTOR, "a mounted disc spins before the Stop");

    stop_until_motor_off();
    CHECK(!(stat_reg & (CDSTAT_MOTOR | CDSTAT_READ | CDSTAT_SEEK | CDSTAT_PLAY)), "M1: Stop clears the motor bit");

    /* First read after the Stop: the motor-off charge, once. */
    const int origin = msf_to_lba(read_min, read_sec, read_sect);
    const int motor_off = apply_speed(psx_cd_seek_delay(origin, NEXT_FILE_LBA, 0, s_source_seek_paused, mode_reg, 0));
    const int first = start_read(cmd, NEXT_FILE_LBA);
    snprintf(what, sizeof what, "M2 %s: the first read after Stop pays the motor-off seek charge (%d, want %d)",
             name, first, motor_off + initial_read_delay_cycles());
    CHECK(first == motor_off + initial_read_delay_cycles(), what);
    CHECK(motor_off >= 33868800, "M2: that charge holds the one-second spin-up");
    CHECK(first > RETRY_WINDOW, "the first attempt cannot deliver inside the retry window");
    CHECK((stat_reg & CDSTAT_MOTOR) && (stat_reg & CDSTAT_SEEK) && reading, "M3: the read start sets the motor bit");

    /* 60 vblanks with no sector: the library gives up on this attempt. */
    int before = reads;
    advance(RETRY_WINDOW + 2 * NTSC_VBLANK_CYCLES);
    CHECK(reads == before && !(irq_flag & 7u), "no sector and no interrupt inside the first window");
    command(0x09); finish();
    CHECK(!reading && (stat_reg & CDSTAT_MOTOR), "M4: Pause before the first sector leaves the motor on");
    CHECK(!(stat_reg & (CDSTAT_READ | CDSTAT_SEEK)), "M4: Pause ends the read and the seek");

    /* The re-issued read: motor on, so no second spin-up, and it delivers. */
    const int origin2 = msf_to_lba(read_min, read_sec, read_sect);
    const int motor_on = apply_speed(psx_cd_seek_delay(origin2, NEXT_FILE_LBA, 1, s_source_seek_paused, mode_reg, 0));
    setmode(0xA0);
    const int second = start_read(cmd, NEXT_FILE_LBA);
    snprintf(what, sizeof what, "M5 %s: the re-issued read is charged with the motor on (%d, want %d)",
             name, second, motor_on + initial_read_delay_cycles());
    CHECK(second == motor_on + initial_read_delay_cycles(), what);
    snprintf(what, sizeof what, "M5 %s: it is due inside the retry window (%d of %d cycles)", name, second, RETRY_WINDOW);
    CHECK(second < RETRY_WINDOW, what);
    before = reads; irq_flag = 0;
    advance(second - 1);
    CHECK(reads == before, "not before its time");
    advance(1);
    CHECK(reads == before + 1 && irq_flag == CDIRQ_DATA_READY, "M5: the first sector arrives on time");
    CHECK(last_sector_lba == NEXT_FILE_LBA, "M5: it is the sector the game asked for");
    CHECK((stat_reg & CDSTAT_MOTOR) && (stat_reg & CDSTAT_READ), "reading with the motor on");
}

/* A read that starts with the motor already on changes nothing. */
static void motor_already_on(void) {
    default_path();
    setmode(0xA0);
    cursor_at(LOGO_END_LBA + 1);
    const int want = apply_speed(psx_cd_seek_delay(LOGO_END_LBA + 1, NEXT_FILE_LBA, 1, 0, mode_reg, 0)) +
                     initial_read_delay_cycles();
    const int got = start_read(0x06, NEXT_FILE_LBA);
    CHECK(got == want && (stat_reg & CDSTAT_MOTOR), "a read with the motor on is timed as before");
}

/* M7: a source profile keeps the status it had. */
static void source_profile_unchanged(const char *drive) {
    char what[160];
    source_profile(drive);
    CHECK(s_source_clock, "the profile runs on the source clock");
    mode_reg = 0x80;
    for (int attempt = 0; attempt < 2; ++attempt) {
        stat_reg = 0;                         /* motor off, as after a Stop */
        reading = 0; s_source_seek_paused = 0; source_drive_head_valid = 0;
        cursor_at(LOGO_END_LBA + 1);
        param_fifo[0] = bin_to_bcd((NEXT_FILE_LBA + 150) / 4500);
        param_fifo[1] = bin_to_bcd((NEXT_FILE_LBA + 150) / 75 % 60);
        param_fifo[2] = bin_to_bcd((NEXT_FILE_LBA + 150) % 75);
        param_count = 3; irq_flag = 0; response_clear();
        exec_command(0x02);                   /* Setloc */
        irq_flag = 0; response_clear();
        exec_command(0x06);
        snprintf(what, sizeof what, "M7 %s: read start %d leaves the motor bit clear", drive, attempt + 1);
        CHECK(reading && !(stat_reg & CDSTAT_MOTOR), what);
        snprintf(what, sizeof what, "M7 %s: read start %d pays the motor-off charge (%d)", drive, attempt + 1, read_delay);
        CHECK(read_delay >= 33868800, what);
        stop_read_stream();
    }
    stat_reg = CDSTAT_MOTOR | CDSTAT_PLAY;    /* and it sets nothing it did not before */
    cursor_at(100); setloc_pending = 0; irq_flag = 0; response_clear();
    exec_command(0x1B);
    CHECK((stat_reg & CDSTAT_MOTOR) && !(stat_reg & CDSTAT_PLAY), "M7: a read with the motor on keeps it on");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <random tape>\n", argv[0]); return 64; }
    tape_path = argv[1];
    retry_after_stop(0x06, "ReadN");
    retry_after_stop(0x1B, "ReadS");          /* M6 */
    motor_already_on();
    source_profile_unchanged("");
    source_profile_unchanged("nymashock-1.29.0");
    source_profile_unchanged("octoshock-2.7");
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: a read start spins the motor up on the default path only");
    return 0;
}
