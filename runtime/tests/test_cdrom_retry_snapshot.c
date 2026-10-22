/* PS1G-140: the CD retry dither and its run in the save state.
 *
 * A game that loses a race against vblank re-issues the same read (Pause,
 * Setloc, ReadN) after a few sectors. From the RETRY_DITHER_AFTER-th re-issue
 * the default path adds a hashed 0..1 sector-period delay to the first sector.
 * The run (target and count) is in the CD section, so a state saved inside
 * such a loop resumes it exactly.
 *
 * Rows:
 *   R1 A stall loop (3 sectors per attempt) fires from the third re-issue on.
 *   R2 Save after the third attempt, reset the controller, load: the later
 *      re-issues get the same delays and the section bytes are equal to the
 *      uninterrupted run's at the end.
 *   R3 Without the run in the state (the count cleared after the load) the
 *      next delay differs: the round trip in R2 is not trivially equal.
 *   R4 The section rejects a target before the lead-in (-151) or past the
 *      disc, and a negative count; it accepts -1 (no run) and a lead-in read
 *      target (-150).
 *   R5 A read that delivered more than RETRY_STALL_SECTORS sectors past its
 *      target before the Pause was progressing, not stalled: its re-issue
 *      restarts the count and gets no dither.
 *   R6 PSX_CD_RETRY_DITHER=0 is read once at the first read start, so it is
 *      covered by the product run, not here.
 * Controller-level, through the MMIO interface. Synthetic sectors only; no
 * BIOS or game data.
 *
 * usage: cdrom_retry_snapshot_test */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

#define TARGET_LBA 33485     /* RE3 Europe, RPD stairwell */
#define ATTEMPTS   7
#define SAVE_AFTER 3         /* attempts finished before the save */

static void default_path(void) {
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
}
static void setmode(uint8_t mode) {
    cdrom_write(0x1f801800, 0); cdrom_write(0x1f801802, mode); command(0x0E); ack();
}
/* One attempt as the library sends it: Setloc, ReadN, `sectors` sectors, Pause.
 * Returns the scheduled delay to the first sector. */
static int attempt(int lba, int sectors) {
    setmode(0x80);
    target(lba);
    command(0x06); ack();
    const int delay = read_delay;
    const int before = reads;
    advance(delay);
    for (int k = 1; k < sectors; ++k) { ack(); advance((int)source_drive_sector_period()); }
    CHECK(reads - before >= sectors, "the attempt delivered its sectors");
    command(0x09); finish();
    return delay;
}
static uint8_t *section(uint32_t *n) {
    *n = cdrom_snapshot_bytes();
    uint8_t *p = malloc(*n);
    if (!p) abort();
    cdrom_snapshot_write(p);
    return p;
}

static int delays_ref[ATTEMPTS];
static uint64_t fires_ref;
static uint8_t *end_ref; static uint32_t end_ref_n;

static void stall_loop_reference(void) {
    char what[160];
    default_path();
    CHECK(!s_source_clock, "the default path has no source clock");
    attempt(TARGET_LBA - 40, 4);                 /* the previous file */
    const uint64_t fires0 = s_cd_retry_dither_count;
    for (int a = 0; a < ATTEMPTS; ++a) delays_ref[a] = attempt(TARGET_LBA, 3);
    fires_ref = s_cd_retry_dither_count - fires0;
    /* Re-issue 1 is attempt 2: attempts 1-3 run undithered, then it fires. */
    snprintf(what, sizeof what, "R1: the stall loop fires from the third re-issue (%llu fires, want %d)",
             (unsigned long long)fires_ref, ATTEMPTS - RETRY_DITHER_AFTER);
    CHECK(fires_ref == (uint64_t)(ATTEMPTS - RETRY_DITHER_AFTER), what);
    CHECK(delays_ref[0] == delays_ref[1] && delays_ref[1] == delays_ref[2], "R1: the first attempts are not dithered");
    int moved = 0;
    for (int a = RETRY_DITHER_AFTER; a < ATTEMPTS; ++a) moved += delays_ref[a] != delays_ref[0];
    CHECK(moved > 0, "R1: a fired attempt moves its first sector");
    end_ref = section(&end_ref_n);
}

static void round_trip(int drop_run, int *first_diff) {
    char what[160];
    default_path();
    attempt(TARGET_LBA - 40, 4);
    for (int a = 0; a < SAVE_AFTER; ++a) attempt(TARGET_LBA, 3);
    uint32_t n; uint8_t *saved = section(&n);
    const uint64_t cycle = psx_cycle_count;
    CHECK(s_retry_target == TARGET_LBA && s_retry_run == SAVE_AFTER - 1, "the save holds a live run");

    cdrom_init("synthetic");                       /* a fresh controller, as at a load */
    CHECK(s_retry_target == -1 && s_retry_run == 0, "power-on has no run");
    psx_cycle_count = cycle;
    CHECK(cdrom_snapshot_read(saved, n), "the saved section loads");
    if (drop_run) { s_retry_target = -1; s_retry_run = 0; }
    else CHECK(s_retry_target == TARGET_LBA && s_retry_run == SAVE_AFTER - 1, "R2: the load restores the run");

    *first_diff = -1;
    for (int a = SAVE_AFTER; a < ATTEMPTS; ++a) {
        const int d = attempt(TARGET_LBA, 3);
        if (d != delays_ref[a] && *first_diff < 0) *first_diff = a;
    }
    if (!drop_run) {
        snprintf(what, sizeof what, "R2: every re-issue after the load has the uninterrupted delay (first differs at %d)", *first_diff);
        CHECK(*first_diff < 0, what);
        uint32_t m; uint8_t *end = section(&m);
        CHECK(m == end_ref_n && !memcmp(end, end_ref, m), "R2: the section bytes at the end equal the uninterrupted run's");
        free(end);
    }
    free(saved);
}

static void validate_rows(void) {
    default_path();
    attempt(TARGET_LBA, 3); attempt(TARGET_LBA, 3);
    uint32_t n; uint8_t *p = section(&n);
    CHECK(cdrom_snapshot_validate(p, n), "R4: a live run validates");
    uint8_t *retry = p + n - 1 - 8;                /* default path: no tails after the seek byte */
    const uint32_t keep_t = cd_tape_le32(retry), keep_r = cd_tape_le32(retry + 4);
    CHECK((int32_t)keep_t == TARGET_LBA && (int32_t)keep_r == 1, "R4: the run sits before the seek byte");
    const int32_t targets[] = { -1, -2, -150, -151, 450000, 450001 };
    const int ok_t[] = { 1, 1, 1, 0, 1, 0 };
    for (int i = 0; i < 6; ++i) {
        retry[0] = (uint8_t)targets[i]; retry[1] = (uint8_t)(targets[i] >> 8);
        retry[2] = (uint8_t)(targets[i] >> 16); retry[3] = (uint8_t)(targets[i] >> 24);
        CHECK(cdrom_snapshot_validate(p, n) == ok_t[i], "R4: the target range is checked");
    }
    retry[0] = (uint8_t)keep_t; retry[1] = (uint8_t)(keep_t >> 8); retry[2] = (uint8_t)(keep_t >> 16); retry[3] = (uint8_t)(keep_t >> 24);
    retry[4] = retry[5] = retry[6] = retry[7] = 0xFF;   /* run -1 */
    CHECK(!cdrom_snapshot_validate(p, n), "R4: a negative count is refused");
    free(p);
}

static void progressing_reads(void) {
    char what[160];
    default_path();
    attempt(TARGET_LBA - 40, 4);
    const uint64_t fires0 = s_cd_retry_dither_count;
    /* A file read in full, then read again from its start, several times. */
    for (int a = 0; a < 5; ++a) attempt(TARGET_LBA, RETRY_STALL_SECTORS + 3);
    snprintf(what, sizeof what, "R5: re-reads after long reads do not fire (%llu fires)",
             (unsigned long long)(s_cd_retry_dither_count - fires0));
    CHECK(s_cd_retry_dither_count == fires0, what);
    /* At the limit it still counts as a stall. */
    default_path();
    attempt(TARGET_LBA - 40, 4);
    const uint64_t fires1 = s_cd_retry_dither_count;
    for (int a = 0; a < 5; ++a) attempt(TARGET_LBA, RETRY_STALL_SECTORS + 1);
    CHECK(s_cd_retry_dither_count - fires1 == (uint64_t)(5 - RETRY_DITHER_AFTER), "R5: a short read at the limit still counts");
}

int main(void) {
    int diff;
    stall_loop_reference();
    round_trip(0, &diff);
    round_trip(1, &diff);
    {
        char what[160];
        snprintf(what, sizeof what, "R3: without the run the first re-issue after the load differs (at %d, want %d)",
                 diff, SAVE_AFTER);
        CHECK(diff == SAVE_AFTER, what);
    }
    validate_rows();
    progressing_reads();
    free(end_ref);
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: the CD retry run survives a save and load");
    return 0;
}
