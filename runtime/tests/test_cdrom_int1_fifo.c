/* A delayed INT1 consumer must drain the announced sector, then the next
 * announced sector, while read-ahead proceeds. Synthetic sectors only.
 * Hardware contract: PSX-SPX, CDROM Incoming Data / Buffer Overrun Timings. */
#define main existing_implicit_seek_main
#include "test_cdrom_implicit_seek.c"
#undef main

static void fifo_case(int whole, int snapshot, int overrun) {
    setup(0);
    mode_reg = whole ? 0xA0 : 0x80;
    target(10);
    command(6);
    int first = read_delay;
    ack();
    advance(first);
    CHECK(last_sector_lba == 10 && irq_flag == 1, "first INT1 announces requested sector");
    advance(225792);
    advance(225792);
    CHECK(last_sector_lba == 12, "two later sectors arrive before the consumer");
    ack();
    advance(568); /* observed legal ACK-before-BFRD gap, below port readiness */
    cdrom_write(0x1f801803, 0x80);
    if (whole) {
        uint32_t header = cdrom_dma_read();
        CHECK(header == 0x02100200u, "DMA header is the requested oldest sector, not newest");
        (void)cdrom_dma_read();
        (void)cdrom_dma_read();
    }
    advance(2000); /* a newer INT1 can now be presented while this FIFO drains */
    if (snapshot) {
        uint32_t bytes = cdrom_snapshot_bytes();
        uint8_t *wire = malloc(bytes);
        CHECK(wire != NULL, "snapshot buffer allocated");
        if (!wire) return;
        cdrom_snapshot_write(wire);
        clear_sector_buffer();
        CHECK(cdrom_snapshot_read(wire, bytes), "active FIFO checkpoint restores");
        free(wire);
    }
    int good = 1;
    for (unsigned word = 0; word < 512; ++word) {
        if (word == 128 && overrun)
            for (unsigned sector = 0; sector < 9; ++sector) advance(225792);
        if (cdrom_dma_read() != 0x0A0A0A0Au) good = 0;
    }
    CHECK(good, "announced payload survives newer INT1 and read-ahead overwrite");
    if (!snapshot && !overrun) {
        cdrom_write(0x1f801803, 0);
        cdrom_write(0x1f801803, 0x80);
        if (whole) {
            CHECK(cdrom_dma_read() == 0x02120200u, "next request selects the next announced sector");
            (void)cdrom_dma_read(); (void)cdrom_dma_read();
        }
        CHECK(cdrom_dma_read() == 0x0C0C0C0Cu, "next request does not replay the locked old FIFO");
    }
}
int main(void) {
    for (int whole = 0; whole != 2; ++whole)
        for (int snapshot = 0; snapshot != 2; ++snapshot)
            for (int overrun = 0; overrun != 2; ++overrun)
                fifo_case(whole, snapshot, overrun);
    printf("INT1 FIFO cases=8 failures=%d snapshot_bytes=%u\n", failures, cdrom_snapshot_bytes());
    return failures ? 1 : 0;
}
