/* Preserve the accepted default Pause/Read position, using the real controller
 * and the maintained synthetic reader/FIFO fixture (no disc or audio device). */
#define main upstream_cdrom_fifo_main
#include "test_cdrom_sector_fifo.c"
#undef main

static void snapshot_round_trip(void) {
    uint32_t bytes = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(bytes);
    CHECK(wire);
    cdrom_snapshot_write(wire);
    exec_command(8); response_clear(); ack(); /* clear eligibility via Stop */
    read_sec = 9;
    CHECK(cdrom_snapshot_read(wire, bytes));
    free(wire);
}

int main(void) {
    unsigned cases = 0;
    const uint8_t commands[] = {0x06, 0x1B};
    const uint8_t modes[] = {0x80, 0xA0};
    for (unsigned c = 0; c < sizeof commands; c++) {
        for (unsigned m = 0; m < sizeof modes; m++) {
            for (unsigned restore = 0; restore < 2; restore++) {
                setup(); mode_reg = modes[m];
                sector(); ack(); /* actual controller delivery: LBA100 */
                if (restore) snapshot_round_trip(); /* active read state */
                exec_command(9); response_clear(); ack();
                if (restore) snapshot_round_trip(); /* paused state */
                exec_command(0x11);
                CHECK(response_count == 8);
                printf("paused-subq cmd=%02x mode=%02x restore=%u msf=%02x:%02x:%02x expected=00:03:25\n",
                       commands[c], modes[m], restore, response_fifo[5], response_fifo[6], response_fifo[7]);
                CHECK(response_fifo[5] == 0 && response_fifo[6] == 3 && response_fifo[7] == 0x25);
                response_clear(); ack();
                exec_command(commands[c]); response_clear(); ack();
                CHECK(msf_to_lba(read_min, read_sec, read_sect) == 100);
                sector(); request();
                if (modes[m] & 0x20) {
                    for (unsigned word = 0; word < 585; word++) {
                        const uint32_t expected = word == 0 ? 0x02250300u :
                            word < 3 ? 0x00080709u : word < 515 ? 0x64646464u : 0;
                        CHECK(cdrom_dma_ready());
                        CHECK(cdrom_dma_read() == expected);
                    }
                } else {
                    drain(100, 512);
                }
                ack();
                /* Repeated Pause must not repeatedly subtract a sector. */
                exec_command(9); response_clear(); ack();
                exec_command(9); response_clear(); ack();
                exec_command(commands[c]); response_clear(); ack();
                CHECK(msf_to_lba(read_min, read_sec, read_sect) == 100);
                cases++;
            }
        }
    }
    /* A new stream paused before its first delivery must not reuse the last
     * stream's metadata, including an adjacent new Setloc target. */
    for (unsigned target = 101; target <= 102; target++) {
        setup(); sector(); ack();
        exec_command(9); response_clear(); ack();
        param_fifo[0] = 0; param_fifo[1] = 3;
        param_fifo[2] = bin_to_bcd(25 + target - 100); param_count = 3;
        exec_command(2); response_clear(); ack();
        exec_command(6); response_clear(); ack();
        snapshot_round_trip();
        exec_command(9); response_clear(); ack();
        exec_command(6); response_clear(); ack();
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == (int)target);
        cases++;
    }
    /* XA sectors do not enter the data FIFO, but still establish a receipt
     * position. Exercise the production decoder through the stub SPU sink. */
    for (unsigned restore = 0; restore < 2; restore++) {
        setup(); mode_reg = 0xC0; test_submode = 0x64;
        sector(); ack();
        CHECK(test_xa_frames > 0);
        if (restore) snapshot_round_trip();
        exec_command(9); response_clear(); ack();
        if (restore) snapshot_round_trip();
        exec_command(0x1B); response_clear(); ack();
        int before = test_xa_frames;
        sector();
        printf("paused-xa restore=%u first_lba=%d expected=100 frames=%d\n",
               restore, last_sector_lba, test_xa_frames - before);
        CHECK(last_sector_lba == 100 && test_xa_frames > before);
        cases++;
    }
    /* Existing resident-completion and new-target/explicit-seek controls are
     * required alongside the last-received-sector correction. */
    resident_header_cursor_and_restore();
    pause_read_resumes_position();
    snapshot_setloc_and_paused_head();
    invalid_setloc_preserves_target();
    /* Older CDF3 wire remains readable without inventing a per-stream
     * receipt bit it never stored. Invalid CDF4 flags cannot mutate state. */
    setup(); sector(); ack();
    const uint32_t bytes = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(bytes), *after = malloc(bytes);
    CHECK(wire && after);
    cdrom_snapshot_write(wire);
    for (unsigned missing = 1; missing < 5; missing++)
        CHECK(!cdrom_snapshot_validate(wire, bytes - missing));
    wire[bytes - 22] = 2; /* actual CDF4 received-sector Boolean */
    CHECK(!cdrom_snapshot_read(wire, bytes));
    wire[bytes - 22] = 1;
    cdrom_snapshot_write(after);
    CHECK(memcmp(wire, after, bytes) == 0);
    for (unsigned offset = bytes - 31; offset <= bytes - 26; offset += 5) {
        wire[offset] ^= 1;
        CHECK(!cdrom_snapshot_read(wire, bytes));
        wire[offset] ^= 1;
        cdrom_snapshot_write(after);
        CHECK(memcmp(wire, after, bytes) == 0);
    }
    CHECK(cdrom_snapshot_read(wire, bytes - 26)); /* actual CDF3 length */
    exec_command(9); response_clear(); ack();
    CHECK(msf_to_lba(read_min, read_sec, read_sect) == 101);
    setup(); sector(); ack();
    PstW legacy;
    pst_w_init(&legacy, wire, bytes);
    CHECK(cdrom_snap_emit(&legacy, 0));
    CHECK(cdrom_snapshot_read(wire, (uint32_t)legacy.written));
    exec_command(9); response_clear(); ack();
    CHECK(msf_to_lba(read_min, read_sec, read_sect) == 101);
    free(after); free(wire);
    printf("PASS: paused-read preservation cases=%u; resident/new-target controls retained\n", cases);
    return 0;
}
