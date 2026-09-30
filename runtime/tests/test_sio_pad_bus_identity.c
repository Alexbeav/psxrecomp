/* Byte identity of the pad and memory card bus.
 *
 * The script below drives the production sio.c through a plain digital pad, a
 * DualShock config handshake with a rumble map, a memory card ID and sector
 * read, and (when PSX_MAX_PLAYERS >= 5) multitap bulk reads. Every exchanged
 * byte (tx, rx, and whether IRQ7 followed) and the final SIO snapshot wire go
 * into one FNV-1a hash, which must match the golden for the build's player
 * count.
 *
 * The goldens were recorded with --print from this file built against the
 * sio.c of the commit that added it. A later change that moves a single byte
 * of pad or card traffic fails here; a change that means to move one must
 * record new goldens and say why.
 *
 * usage: <exe> [--print] [--trace]
 *   --print  print the hash instead of checking it
 *   --trace  also print every transaction's reply bytes
 * One run per process: sio_init deliberately keeps its diagnostic sequence
 * counters, so a second run in the same process would not start from power-on.
 */
#include "../src/sio.c"
#include <stdio.h>
#include <stdlib.h>

uint32_t i_stat, i_mask, g_debug_current_func_addr, g_debug_last_store_pc;
static uint64_t clock_now;

uint64_t psx_get_cycle_count(void) { return clock_now; }
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t a) { (void)a; return 0; }
uint32_t psx_read_word(uint32_t a) { (void)a; return 0; }
uint32_t memory_get_sr(void) { return 0; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void starvation_ring_record(uint8_t a, uint8_t b, uint8_t c, uint16_t d, uint16_t e,
    int f, int g, int h, int i, int j, uint8_t k, uint32_t l,
    uint8_t m, uint8_t n, uint8_t o, uint8_t p, int q) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    (void)h; (void)i; (void)j; (void)k; (void)l;
    (void)m; (void)n; (void)o; (void)p; (void)q;
}
void card_read_summary_record(uint8_t a, uint8_t b, uint16_t c, uint8_t d,
    uint8_t e, const uint8_t *f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
void card_data_writes_arm(uint8_t a, uint16_t b, uint8_t c, uint8_t d) {
    (void)a; (void)b; (void)c; (void)d;
}
int memcard_is_present(int slot) { return slot == 0; }
int memcard_read_sector(int slot, int sector, uint8_t *buf) {
    (void)slot;
    for (int i = 0; i < 128; i++) buf[i] = (uint8_t)(sector + i);
    return 0;
}
int memcard_write_sector(int slot, int sector, const uint8_t *buf) {
    (void)slot; (void)sector; (void)buf; return 0;
}
void memcard_flush(int slot) { (void)slot; }

static uint64_t hash;
static unsigned bytes;
static int trace;

static void mix(uint8_t v) { hash ^= v; hash *= 0x100000001B3ull; }
static void advance(unsigned n) { while (n--) { clock_now++; sio_advance(1); } }

/* One transaction on a port: every byte's tx, rx and IRQ7 go into the hash. */
static void txn(int port, const uint8_t *tx, unsigned count) {
    const uint32_t slot = port ? 0x2000u : 0u;
    if (trace) printf("port %d:", port + 1);
    sio_write(0x1F80104A, 0);
    sio_write(0x1F80104A, 0x1003u | slot);
    for (unsigned i = 0; i < count; ++i) {
        sio_write(0x1F801040, tx[i]);
        advance(1088);
        const uint8_t rx = (uint8_t)sio_read(0x1F801040);
        advance(256);
        const uint8_t irq = (uint8_t)((i_stat >> 7) & 1u);
        mix(tx[i]); mix(rx); mix(irq); bytes++;
        if (trace) printf(" %02X%s", rx, irq ? "+" : "");
        sio_write(0x1F80104A, 0x1013u | slot);
        i_stat &= ~0x80u;
    }
    if (trace) printf("\n");
    sio_write(0x1F80104A, 0);
    advance(2000);
}
#define TXN(port, ...) do { static const uint8_t t_[] = { __VA_ARGS__ }; \
                            txn(port, t_, sizeof t_); } while (0)

static void script(void) {
    /* Port 1: plain digital pad, not config-capable. */
    sio_set_pad_connected(0, 1); sio_set_pad_config_capable(0, 0);
    sio_set_pad_analog(0, 0, 0x80, 0x80, 0x80, 0x80);
    /* Port 2: DualShock, analog, config-capable. */
    sio_set_pad_connected(1, 1); sio_set_pad_config_capable(1, 1);
    sio_set_pad_analog(1, 1, 0x10, 0xE0, 0x7F, 0x81);
    sio_write(0x1F801048, 0xD); sio_write(0x1F80104E, 0x88);

    for (int f = 0; f < 4; ++f) {
        sio_set_pad_state_slot(0, (uint16_t)(0xFFFFu ^ (1u << (4 + f))));
        sio_set_pad_state_slot(1, (uint16_t)(0xFFFFu ^ (1u << (12 + f))));
        TXN(0, 0x01, 0x42, 0x00, 0x00, 0x00);
        TXN(1, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    }
    /* The digital pad ignores config probes. */
    TXN(0, 0x01, 0x43, 0x00, 0x01, 0x00);
    TXN(0, 0x01, 0x45, 0x00, 0x00, 0x00);
    /* DualShock config handshake, queries, rumble map, lock, exit. */
    TXN(1, 0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    TXN(1, 0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    TXN(1, 0x01, 0x46, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    TXN(1, 0x01, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    TXN(1, 0x01, 0x4C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    TXN(1, 0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF);
    TXN(1, 0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00);
    TXN(1, 0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    TXN(1, 0x01, 0x42, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x00);
    sio_request_pad_type(1, 0);
    TXN(1, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    /* Memory card on port 1: get ID, then read sector 3. */
    TXN(0, 0x81, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    {
        uint8_t rd[140] = { 0x81, 0x52, 0x00, 0x00, 0x00, 0x03 };
        txn(0, rd, sizeof rd);
    }
    /* An unknown address on port 2, then a disconnected port. */
    TXN(1, 0x02, 0x42, 0x00);
    sio_set_pad_connected(1, 0);
    TXN(1, 0x01, 0x42, 0x00, 0x00, 0x00);
    sio_set_pad_connected(1, 1);
    TXN(1, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
#if PSX_MAX_PLAYERS >= 5
    /* Multitap on port 1: seats A-D, the REQ latch, bulk and garbage replies. */
    sio_set_multitap_port(0); sio_set_multitap(1);
    for (int s = 0; s < 4; ++s) {
        sio_set_pad_connected(s, 1);
        sio_set_pad_config_capable(s, 0);
        sio_set_pad_state_slot(s, (uint16_t)(0xFFFFu ^ (1u << (s + 4))));
    }
    for (int r = 0; r < 3; ++r) {
        TXN(0, 0x01, 0x42, 0x01, 0x00, 0x00);
        TXN(0, 0x01, 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    }
    TXN(0, 0x03, 0x42, 0x00, 0x00, 0x00);
    sio_set_multitap(0);
#endif
    /* Final snapshot wire. */
    {
        const uint32_t n = sio_snapshot_bytes();
        uint8_t *p = (uint8_t *)calloc(n ? n : 1, 1);
        if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
        if (n) sio_snapshot_write(p);
        mix((uint8_t)n); mix((uint8_t)(n >> 8));
        for (uint32_t i = 0; i < n; ++i) mix(p[i]);
        free(p);
    }
}

/* Recorded with --print against the sio.c this file was added with
 * (sha256 18b77e19c9332807d2a827c9ac61a50309121bf0d590732db3995c4aae6b231f).
 * GOLDEN2: PSX_MAX_PLAYERS=2 build, 323 bytes. GOLDEN5: PSX_MAX_PLAYERS=5
 * (multitap) build, 445 bytes. */
#define GOLDEN2 0x8C04005D77A6E414ull
#define GOLDEN5 0x81169F9AABC77675ull

int main(int argc, char **argv) {
    int print = 0;
    for (int a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "--print")) print = 1;
        else if (!strcmp(argv[a], "--trace")) trace = 1;
        else { fprintf(stderr, "usage: %s [--print] [--trace]\n", argv[0]); return 2; }
    }
    const uint64_t golden = PSX_MAX_PLAYERS >= 5 ? GOLDEN5 : GOLDEN2;
    hash = 0xCBF29CE484222325ull;
    sio_init();
    script();
    if (print) {
        printf("players=%d bytes=%u hash=0x%016llXull\n",
               PSX_MAX_PLAYERS, bytes, (unsigned long long)hash);
        return 0;
    }
    if (hash != golden) {
        fprintf(stderr, "FAIL: players=%d: hash %016llX, golden %016llX (%u bytes)\n",
                PSX_MAX_PLAYERS, (unsigned long long)hash,
                (unsigned long long)golden, bytes);
        return 1;
    }
    printf("sio_pad_bus_identity: players=%d: %u bytes match\n", PSX_MAX_PLAYERS, bytes);
    return 0;
}
