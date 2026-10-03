/* PS1B-424: the rows of spec section 14 (command reception and Init on the
 * source clock, recomp-corpus references/ps1/CDROM-SOURCE-PROFILE-SPEC.md),
 * one by one, through the controller's register interface.
 *
 * Every expected value comes from a row of section 14: the numbers 12,315,
 * 3,000, 1,815, 8,500, 2,000 and 1,136,000 and the order of the steps. The
 * oracle fixtures (cdrom_c_fixture_replay_test) compare within 500 cycles;
 * this test compares each time exactly. Each check names its row.
 *
 * It reuses the stubs and helpers of test_cdrom_source_explicit_timing.c, as
 * test_cdrom_source_reset.c does, and reaches the controller through the
 * functions and variables that those tests use. Two of the five state values
 * of spec 14.1, s_source_command_phase and s_source_args_remaining, are read
 * by no older test; the checks that read them are left out when
 * CD_CLOCK_ROWS_NO_STATE_NAMES is defined. Their effect is checked in any
 * case, through the due times.
 *
 * usage: cdrom_source_clock_rows_test <tape>
 * tape: runtime/tests/cd_source_cold_random_256.psxrng */
#define main source_explicit_baseline_main
#include "test_cdrom_source_explicit_timing.c"
#undef main

static const char *tape_path;
static unsigned checks;
#define EXPECT(cond, ...) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#ifndef CD_CLOCK_ROWS_NO_STATE_NAMES
#define STATE(phase, args, what) EXPECT(s_source_command_phase == (phase) && s_source_args_remaining == (args), \
    "%s: phase %d, arguments left %d, want %d and %d", what, s_source_command_phase, s_source_args_remaining, (phase), (args))
#else
#define STATE(phase, args, what) do { } while (0)
#endif

/* A cold controller with the source clock on and no drive profile. */
static void clock_boot(int disc)
{
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", "octoshock-2.2.2");
    set_model("PSX_CD_TOC_SEEK_MODEL", "octoshock-2.2.2");
    set_model("PSX_CD_READ_START_MODEL", "octoshock-2.2.2-pipeline");
    set_model("PSX_CD_SOURCE_CLOCK_TAPE", tape_path);
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0;
    iso_handle = NULL;
    cdrom_init(disc ? "synthetic" : NULL);
    i_stat = 0;
}
static void write_command(uint8_t cmd, const uint8_t *args, int count)
{
    cdrom_write(0x1f801800, 0);
    for (int i = 0; i < count; ++i) cdrom_write(0x1f801802, args[i]);
    cdrom_write(0x1f801801, cmd);
}
static void acknowledge(uint8_t bits)
{
    cdrom_write(0x1f801800, 1);
    cdrom_write(0x1f801803, bits);
    cdrom_write(0x1f801800, 0);
}
/* The next draw in [0, 3000] and the tape cursor after it, without taking it. */
static uint32_t next_draw(uint32_t *cursor_after)
{
    CdRandomTape peek = s_source_clock_tape;
    uint32_t value = 0;
    int taken = cd_tape_bounded(&peek, 3000, &value);
    EXPECT(taken, "the tape has a draw left");
    *cursor_after = peek.cursor;
    return value;
}
/* Run the reception of the queued command until it executes. The status byte
 * of the cycle before each step is kept: the last one is the status from
 * before the command. */
static uint8_t status_before;
static void run_to_execution(void)
{
    for (int guard = 0; queued_cmd.pending && guard < 40; ++guard) {
        uint64_t due = s_source_command_due > s_source_ready_due ? s_source_command_due : s_source_ready_due;
        if (due > psx_cycle_count + 1) advance((int)(due - psx_cycle_count) - 1);
        status_before = stat_reg;
        advance(1);
    }
    EXPECT(!queued_cmd.pending, "the queued command executes");
}

/* ---- 14.2 Q1, Q2 and 14.3 Q3 to Q5: one command with n arguments -----------
 * `late` makes every step come some cycles after its due time: row Q4 counts
 * the next step from the cycle of the call, not from the due time. */
static void test_reception(uint8_t cmd, const uint8_t *args, int count, int late)
{
    char what[64];
    snprintf(what, sizeof what, "command %02X, %d arguments%s", cmd, count, late ? ", late calls" : "");
    clock_boot(1);
    advance(5000 + 777 * count);
    uint32_t cursor, draw = next_draw(&cursor);
    uint64_t written = psx_cycle_count;
    write_command(cmd, args, count);
    /* Q1: the write queues the command with its arguments and executes nothing. */
    EXPECT(queued_cmd.pending && queued_cmd.cmd == cmd && queued_cmd.param_count == count && irq_flag == 0 &&
           response_count == 0, "Q1 %s: queued %d, flag %d", what, queued_cmd.pending, irq_flag);
    /* Q2: the first step is due 12,315 cycles plus one draw after the write. */
    EXPECT(s_source_command_due == written + 12315u + draw, "Q2 %s: first step due %llu after the write, want %u",
           what, (unsigned long long)(s_source_command_due - written), 12315u + draw);
    EXPECT(s_source_clock_tape.cursor == cursor, "Q2 %s: one draw at the write (cursor %u, want %u)", what,
           s_source_clock_tape.cursor, cursor);
    STATE(-1, count, what);
    /* Q3: nothing before the step is due. */
    advance((int)(s_source_command_due - psx_cycle_count) - 1);
    EXPECT(queued_cmd.pending && s_source_command_due == written + 12315u + draw, "Q3 %s: no step one cycle early", what);
    STATE(-1, count, what);
    advance(1 + (late ? 37 : 0));
    for (int taken = 1; taken <= count; ++taken) {
        /* Q4: one argument for each call; the next step 1,815 cycles after this cycle. */
        EXPECT(queued_cmd.pending && irq_flag == 0 && s_source_command_due == psx_cycle_count + 1815u,
               "Q4 %s: argument %d: next step in %lld cycles, want 1815", what, taken,
               (long long)(s_source_command_due - psx_cycle_count));
        STATE(0, count - taken, what);
        uint64_t due = s_source_command_due;
        advance(0);                               /* a second call in the same cycle makes no step */
        advance((int)(due - psx_cycle_count) - 1);
        EXPECT(s_source_command_due == due, "Q3 %s: no step before argument %d is due", what, taken + 1);
        STATE(0, count - taken, what);
        advance(1 + (late ? 11 * taken : 0));
    }
    /* Q4: no argument left: the command may execute 8,500 cycles after this cycle. */
    EXPECT(queued_cmd.pending && irq_flag == 0 && s_source_command_due == psx_cycle_count + 8500u,
           "Q4 %s: last step: next step in %lld cycles, want 8500", what, (long long)(s_source_command_due - psx_cycle_count));
    STATE(1, 0, what);
    advance(8499);
    EXPECT(queued_cmd.pending && irq_flag == 0, "Q5 %s: no execution one cycle early", what);
    advance(1);
    /* Q5: in phase 1 the command executes; all three commands answer with INT3. */
    EXPECT(!queued_cmd.pending && irq_flag == CDIRQ_ACK && response_count == 1, "Q5 %s: executed: queued %d, flag %d", what,
           queued_cmd.pending, irq_flag);
    if (!late)
        EXPECT(psx_cycle_count - written == 12315u + draw + 1815u * (unsigned)count + 8500u,
               "Q5 %s: executed %llu cycles after the write, want %u", what, (unsigned long long)(psx_cycle_count - written),
               12315u + draw + 1815u * (unsigned)count + 8500u);
    EXPECT(s_source_clock_tape.cursor == cursor, "Q2 %s: no draw after the write", what);
    /* SPEC-QUESTIONS point 1: no row changes the state when the command executes. */
    STATE(1, 0, what);
    acknowledge(0x1f);
}

/* ---- 14.2 Q2: the draw is one request with the inclusive maximum 3,000 -------
 * The tape words are set by hand. The bounded reader (CD-ENTROPY-CONTRACT.md,
 * "Request and range interface") masks a word with 4095 for this maximum and
 * passes over a result above the maximum. */
static void put_word(uint32_t index, uint32_t value)
{
    uint8_t *word = s_source_clock_tape.bytes + (size_t)index * 4u;
    word[0] = (uint8_t)value; word[1] = (uint8_t)(value >> 8);
    word[2] = (uint8_t)(value >> 16); word[3] = (uint8_t)(value >> 24);
}
static void test_draw_range(void)
{
    clock_boot(1);
    advance(10);
    uint32_t at = s_source_clock_tape.cursor;
    put_word(at, 3001u);                          /* above the maximum: passed over */
    put_word(at + 1u, 0x00012BB8u);               /* 2BB8h and 4095 = 3000: the maximum itself */
    put_word(at + 2u, 0x00005000u);               /* 5000h and 4095 = 0 */
    uint64_t written = psx_cycle_count;
    write_command(0x01, NULL, 0);
    EXPECT(s_source_command_due == written + 12315u + 3000u && s_source_clock_tape.cursor == at + 2u,
           "Q2 the draw has the maximum 3,000: due %lld after the write, cursor %u", (long long)(s_source_command_due - written),
           s_source_clock_tape.cursor - at);
    run_to_execution();
    acknowledge(0x1f);
    written = psx_cycle_count;
    write_command(0x01, NULL, 0);
    EXPECT(s_source_command_due == written + 12315u && s_source_clock_tape.cursor == at + 3u,
           "Q2 a draw of 0: due %lld after the write", (long long)(s_source_command_due - written));
}

/* ---- 14.2 Q2: a second command write starts the reception again ------------- */
static void test_second_write(void)
{
    static const uint8_t mode[1] = { 0x00 };
    uint32_t cursor;
    clock_boot(1);
    advance(100);
    write_command(0x0e, mode, 1);                 /* Setmode, one argument */
    advance((int)(s_source_command_due - psx_cycle_count));
    STATE(0, 0, "after the argument step");
    uint32_t draw = next_draw(&cursor);
    uint64_t written = psx_cycle_count;
    write_command(0x01, NULL, 0);                 /* GetStat replaces it */
    EXPECT(queued_cmd.pending && queued_cmd.cmd == 0x01 && s_source_command_due == written + 12315u + draw &&
           s_source_clock_tape.cursor == cursor, "Q2 a second write draws again and starts again");
    STATE(-1, 0, "after the second write");
    draw = next_draw(&cursor);
    advance(3000);
    written = psx_cycle_count;
    write_command(0x0c, NULL, 0);                 /* Demute, written before any step */
    EXPECT(queued_cmd.cmd == 0x0c && s_source_command_due == written + 12315u + draw && s_source_clock_tape.cursor == cursor,
           "Q2 one draw for each command write");
    run_to_execution();
    EXPECT(irq_flag == CDIRQ_ACK && psx_cycle_count - written == 12315u + draw + 8500u, "Q5 the last written command executes");
}

/* ---- 14.3 Q3 and 14.4 Q6 to Q8: the interrupt flag and the wait after an
 * acknowledge --------------------------------------------------------------- */
static void test_blocked_reception(void)
{
    uint32_t cursor;
    clock_boot(1);
    write_command(0x01, NULL, 0);
    run_to_execution();
    EXPECT(irq_flag == CDIRQ_ACK && s_source_ready_due == 0, "Q7 setup: INT3 raised, no wait");
    uint32_t draw = next_draw(&cursor);
    uint64_t written = psx_cycle_count;
    write_command(0x01, NULL, 0);                 /* written while INT3 is not acknowledged */
    uint64_t due = written + 12315u + draw;
    EXPECT(s_source_command_due == due, "Q2 a write under a set flag still starts the reception");
    /* Q3: no step while an interrupt flag is set, however late. */
    advance(60000);
    EXPECT(queued_cmd.pending && s_source_command_due == due, "Q3 no step while an interrupt flag is set");
    STATE(-1, 0, "under a set flag");
    /* Q8: an acknowledge that leaves a flag bit set starts no wait. */
    acknowledge(0x01);
    EXPECT(irq_flag == 2 && s_source_ready_due == 0, "Q8 partial acknowledge: flag %d, wait until %llu", irq_flag,
           (unsigned long long)s_source_ready_due);
    advance(100);
    EXPECT(s_source_command_due == due, "Q3 no step after a partial acknowledge");
    /* Q8: the acknowledge that clears the last bit: ready 2,000 cycles later. */
    uint64_t cleared = psx_cycle_count;
    acknowledge(0x02);
    EXPECT(irq_flag == 0 && s_source_ready_due == cleared + 2000u, "Q8 wait until %lld cycles after the acknowledge, want 2000",
           (long long)(s_source_ready_due - cleared));
    /* Q3 with Q6: the step is overdue, but the port is not ready. */
    EXPECT(s_source_command_due == due, "Q3 no step at the acknowledge itself");
    advance(1999);
    EXPECT(s_source_command_due == due, "Q3/Q6 no step 1,999 cycles after the acknowledge");
    /* Q8: a write with no flag set before it starts no new wait. */
    acknowledge(0x1f);
    EXPECT(s_source_ready_due == cleared + 2000u, "Q8 an acknowledge with no flag set changes nothing");
    advance(1);
    EXPECT(s_source_command_due == psx_cycle_count + 8500u, "Q3/Q6 the step comes 2,000 cycles after the acknowledge");
    STATE(1, 0, "after the wait");
    advance(8500);
    /* Q7: raising the interrupt ends the wait: the value is 0 again. */
    EXPECT(irq_flag == CDIRQ_ACK && s_source_ready_due == 0, "Q7 wait value after a raise: %llu, want 0",
           (unsigned long long)s_source_ready_due);
}

/* ---- 14.4 Q6: a second response waits for the port ---------------------------
 * SeekP answers INT3 and, after the seek time, INT2 (PSX-SPX "SeekP"). The
 * INT3 is left unacknowledged until the second response is due. */
static void test_second_response_waits(void)
{
    static const uint8_t position[3] = { 0x00, 0x02, 0x04 };
    clock_boot(1);
    write_command(0x02, position, 3);             /* Setloc 00:02:04 */
    run_to_execution();
    acknowledge(0x1f);
    write_command(0x16, NULL, 0);                 /* SeekP */
    run_to_execution();
    EXPECT(irq_flag == CDIRQ_ACK && pending.pending, "Q6 setup: SeekP acknowledged, second response pending");
    advance((int)(pending.due_cyc - psx_cycle_count) + 5000);
    EXPECT(irq_flag == CDIRQ_ACK && pending.pending, "Q6 the second response waits behind a set flag");
    uint64_t cleared = psx_cycle_count;
    acknowledge(0x1f);
    EXPECT(irq_flag == 0 && pending.pending && s_source_ready_due == cleared + 2000u,
           "Q6 the second response is not presented at the acknowledge");
    advance(1999);
    EXPECT(irq_flag == 0 && pending.pending, "Q6 not presented 1,999 cycles after the acknowledge");
    advance(1);
    EXPECT(irq_flag == CDIRQ_COMPLETE && !pending.pending, "Q6 presented 2,000 cycles after the acknowledge (flag %d)", irq_flag);
    EXPECT(s_source_ready_due == 0, "Q7 the raise ends the wait");
}

/* ---- 14.5 J2 to J4: Init with the source clock and no drive profile ---------- */
static void test_init(int disc)
{
    uint32_t cursor;
    uint8_t after = disc ? CDSTAT_MOTOR : CDSTAT_SHELL;
    clock_boot(disc);
    advance(40000);
    stat_reg = (uint8_t)(after | CDSTAT_SEEKERR); /* a status that Init must change */
    next_draw(&cursor);
    write_command(0x0a, NULL, 0);
    run_to_execution();
    uint64_t start = psx_cycle_count;
    /* J2: the first response is the status from before the command, with INT3. */
    EXPECT(irq_flag == CDIRQ_ACK && response_count == 1 && response_fifo[response_read] == status_before &&
           (status_before & CDSTAT_SEEKERR), "J2 disc %d: flag %d, response %02X, status before %02X", disc, irq_flag,
           response_fifo[response_read], status_before);
    /* J3: the reset completes 1,136,000 cycles after this cycle; the status is
     * the motor bit alone with a disc, the shell-open bit alone without. */
    EXPECT(s_source_reset_due == start + 1136000u, "J3 disc %d: reset due %lld cycles after Init, want 1136000", disc,
           (long long)(s_source_reset_due - start));
    EXPECT(stat_reg == after, "J3 disc %d: status %02X, want %02X", disc, stat_reg, after);
    EXPECT(s_source_clock_tape.cursor == cursor, "Init takes no draw of its own (cursor %u, want %u)",
           s_source_clock_tape.cursor, cursor);
    acknowledge(0x1f);

    /* J4: an Init while the reset runs changes neither the time nor the
     * status. J2 still holds for it. */
    advance(300000);
    stat_reg |= CDSTAT_SEEKERR;
    write_command(0x0a, NULL, 0);
    run_to_execution();
    EXPECT(irq_flag == CDIRQ_ACK && response_fifo[response_read] == status_before && (status_before & CDSTAT_SEEKERR),
           "J2 disc %d, second Init: flag %d, response %02X", disc, irq_flag, response_fifo[response_read]);
    EXPECT(s_source_reset_due == start + 1136000u, "J4 disc %d: the reset is not started again", disc);
    EXPECT(stat_reg == status_before, "J4 disc %d: status %02X, want %02X unchanged", disc, stat_reg, status_before);
    acknowledge(0x1f);

    /* J5 (kept code): the completion comes at the time J3 set, not earlier. */
    advance((int)(start + 1135999u - psx_cycle_count));
    EXPECT(irq_flag == 0 && s_source_reset_due == start + 1136000u, "J3 disc %d: no completion one cycle early", disc);
    advance(1);
    EXPECT(irq_flag == CDIRQ_COMPLETE && s_source_reset_due == 0, "J3/J5 disc %d: completion at 1,136,000 cycles (flag %d)",
           disc, irq_flag);
    acknowledge(0x1f);

    /* J3 again: with no reset running, Init starts one. */
    stat_reg = (uint8_t)(after | CDSTAT_SEEKERR);
    write_command(0x0a, NULL, 0);
    run_to_execution();
    EXPECT(s_source_reset_due == psx_cycle_count + 1136000u && stat_reg == after,
           "J3 disc %d: an Init after the completion starts a new reset", disc);
}

/* ---- 14.1: the kept checkpoint code takes the state as the stubs leave it ---- */
static void test_checkpoint(void)
{
    static const uint8_t position[3] = { 0x00, 0x02, 0x10 };
    clock_boot(1);
    advance(900);
    write_command(0x02, position, 3);
    advance((int)(s_source_command_due - psx_cycle_count));       /* one argument taken */
    advance(700);
    unsigned size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size);
    uint64_t saved_cycle = psx_cycle_count, saved_due = s_source_command_due;
    cdrom_snapshot_write(wire);
    run_to_execution();
    uint64_t executed = psx_cycle_count;
    acknowledge(0x1f);
    advance(50);
    /* The deadlines are absolute cycles, so the clock goes back with the state. */
    psx_cycle_count = saved_cycle;
    EXPECT(cdrom_snapshot_read(wire, size), "14.1 a checkpoint in the middle of a reception is accepted");
    EXPECT(queued_cmd.pending && s_source_command_due == saved_due && s_source_ready_due == 0, "14.1 the reception is restored");
    STATE(0, 2, "restored reception");
    run_to_execution();
    EXPECT(psx_cycle_count == executed && irq_flag == CDIRQ_ACK, "14.1 the restored reception executes at the same cycle");
    /* In the wait after an acknowledge, and with a reset running. */
    acknowledge(0x1f);
    write_command(0x0a, NULL, 0);
    run_to_execution();
    acknowledge(0x1f);
    advance(1000);
    cdrom_snapshot_write(wire);
    uint64_t ready = s_source_ready_due, reset = s_source_reset_due;
    saved_cycle = psx_cycle_count;
    advance(5000);
    psx_cycle_count = saved_cycle;
    EXPECT(cdrom_snapshot_read(wire, size) && s_source_ready_due == ready && s_source_reset_due == reset && ready && reset,
           "14.1 a checkpoint in the wait and in a reset is accepted");
    free(wire);
}

/* ---- without the source clock nothing of section 14 applies ------------------ */
static void test_default_path(void)
{
    set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    psx_cycle_count = 0;
    iso_handle = NULL;
    cdrom_init("synthetic");
    advance(1000);
    command(0x01);
    EXPECT(irq_flag == CDIRQ_ACK && !queued_cmd.pending, "default path: a command executes at its write");
    acknowledge(0x1f);
    EXPECT(s_source_ready_due == 0 && s_source_command_due == 0 && s_source_reset_due == 0,
           "default path: Q2, Q7 and Q8 do not apply");
    STATE(-1, 0, "default path");
    command(0x0a);
    EXPECT(irq_flag == CDIRQ_ACK && s_source_reset_due == 0 && pending.pending &&
           pending.due_cyc == psx_cycle_count + 131072u, "default path: Init keeps its own second response");
}

int main(int argc, char **argv)
{
    static const uint8_t mode[1] = { 0x80 }, filter[2] = { 0x01, 0x02 }, position[3] = { 0x00, 0x02, 0x10 };
    if (argc < 2) { fprintf(stderr, "usage: %s <tape>\n", argv[0]); return 2; }
    tape_path = argv[1];
    for (int late = 0; late < 2; ++late) {
        test_reception(0x01, NULL, 0, late);      /* GetStat */
        test_reception(0x0e, mode, 1, late);      /* Setmode */
        test_reception(0x0d, filter, 2, late);    /* Setfilter */
        test_reception(0x02, position, 3, late);  /* Setloc */
    }
    test_draw_range();
    test_second_write();
    test_blocked_reception();
    test_second_response_waits();
    test_init(1);
    test_init(0);
    test_checkpoint();
    test_default_path();
    printf("CD source clock rows (spec 14 Q1-Q8, J2-J4): %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
