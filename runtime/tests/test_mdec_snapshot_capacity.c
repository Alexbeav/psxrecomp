/* Authored MMIO/DMA snapshot controls. Include the owning implementation so
 * the old load can fail at its capacity invariant before an out-of-bounds write. */
#include "../src/mdec.c"

uint64_t s_frame_count;
uint64_t psx_cycle_count;

static void check(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); exit(1); }
}

static void fresh(void) {
    free(mdec.input);
    free(mdec.output);
    mdec_init();
}

static void input_word(unsigned dma, uint32_t value) {
    if (dma) mdec_dma_write_word(value);
    else mdec_write(0, value);
}

static void interrupted_input(unsigned dma, unsigned prepare) {
    const unsigned words = 768, prefix = 16;
    fresh();
    mdec_write(0, (1u << 29) | (1u << 27) | words);
    for (unsigned i = 0; i < prefix; ++i) input_word(dma, 0xFE000000u);
    check(mdec.busy && mdec.input_count < mdec.expected_halfwords,
          "snapshot is inside an input command");
    uint32_t size = mdec_snapshot_bytes();
    uint8_t *saved = malloc(size);
    check(saved != NULL, "snapshot allocation");
    mdec_snapshot_write(saved);
    for (unsigned i = prefix; i < words; ++i) input_word(dma, 0xFE000000u);
    uint32_t completed_size = mdec_snapshot_bytes();
    uint8_t *expected = malloc(completed_size), *actual = malloc(completed_size);
    check(expected && actual, "completed snapshot allocation");
    mdec_snapshot_write(expected);

    fresh(); /* No buffer survives from the process that saved the command. */
    if (prepare) {
        check(mdec_snapshot_prepare(saved, size), "prepare mid-command snapshot");
        check(mdec.input_cap >= words * 2u, "prepare reserves the entire command");
    }
    check(mdec_snapshot_read(saved, size), "read mid-command snapshot");
    check(mdec.input_cap >= mdec.expected_halfwords,
          "read reserves the entire command before continued writes");
    for (unsigned i = prefix; i < words; ++i) input_word(dma, 0xFE000000u);
    check(mdec_snapshot_bytes() == completed_size, "resumed output size");
    mdec_snapshot_write(actual);
    check(memcmp(expected, actual, completed_size) == 0,
          "resumed command equals uninterrupted command");
    free(saved); free(expected); free(actual);
}

static void malformed_and_output(void) {
    fresh();
    mdec_write(0, (1u << 29) | (1u << 27) | 1u);
    mdec_write(0, 0xFE000000u);
    check(mdec.output_size == 64u, "authored pending output");
    (void)mdec_read(0);
    uint32_t size = mdec_snapshot_bytes();
    uint8_t *saved = malloc(size);
    check(saved != NULL, "output snapshot allocation");
    mdec_snapshot_write(saved);
    fresh();
    check(mdec_snapshot_prepare(saved, size), "prepare pending output");
    check(mdec.output_cap >= 64u, "prepare reserves saved output");
    check(mdec_snapshot_read(saved, size), "read pending output");
    check(mdec.output_cap >= mdec.output_size, "read reserves saved output");
    for (unsigned i = 4; i < 64; i += 4)
        check(mdec_read(0) == 0x80808080u, "pending output drains after restore");
    check(!mdec_dma_read_ready(), "output drain completes");

    /* Change only named wire fields in an otherwise valid authored snapshot. */
    PstW w;
    pst_w_init(&w, saved + 8, 4);
    check(pst_w_u32(&w, MDEC_SNAP_INPUT_MAX + 1u), "patch expected length");
    check(!mdec_snapshot_prepare(saved, size), "prepare refuses oversized expected length");
    check(!mdec_snapshot_read(saved, size), "read refuses oversized expected length");
    pst_w_init(&w, saved + 8, 4);
    check(pst_w_u32(&w, 2u), "restore expected length");
    pst_w_init(&w, saved + 48, 4); /* output_pos follows 12 wire uint32 fields. */
    check(pst_w_u32(&w, 65u), "patch output position");
    check(!mdec_snapshot_prepare(saved, size), "prepare refuses output position past size");
    check(!mdec_snapshot_read(saved, size), "read refuses output position past size");
    pst_w_init(&w, saved + 48, 4);
    check(pst_w_u32(&w, 4u), "restore output position");
    pst_w_init(&w, saved + mdec_snap_fixed_bytes() - 12u, 4);
    check(pst_w_u32(&w, MDEC_SNAP_OUTPUT_MAX + 1u), "patch output size");
    check(!mdec_snapshot_prepare(saved, size), "prepare refuses oversized output");
    check(!mdec_snapshot_read(saved, size), "read refuses oversized output");
    free(saved);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "output") == 0) malformed_and_output();
    else {
        for (unsigned dma = 0; dma < 2; ++dma)
            for (unsigned prepare = 0; prepare < 2; ++prepare)
                interrupted_input(dma, prepare);
        malformed_and_output();
    }
    fresh();
    printf("PASS: MDEC snapshot capacity and output bounds\n");
    return 0;
}
