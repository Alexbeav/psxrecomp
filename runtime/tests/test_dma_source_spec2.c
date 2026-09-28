/* Source-profile DMA behaviour (SPEC-PS1B-186-DMA-BEHAVIOUR-v2, evidence/T172/
 * clean-rewrite-specs-20260926). One case per behaviour; each names its spec
 * section. Only the register, clock and service-point entry points are used,
 * so the file also runs against earlier models. Authored; no BIOS or disc. */
#define _POSIX_C_SOURCE 200809L
#include "../src/dma.c"
#include <assert.h>
uint64_t s_frame_count, psx_cycle_count, psx_next_service_cycle;
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t g_io_openbus_reads, g_io_openbus_writes;
static uint32_t ram[0x80000], gp0_words, gp0_last, gpuread_next, spu_in, spu_reads, cd_reads, irqs;
static uint32_t ll_nodes_fetched, ready_after_nodes = 0xFFFFFFFFu, ready_after_words = 0xFFFFFFFFu;
static int gpu_ready = 1, mdec_on, mdec_out_ready, mdec_in_ready;
static uint32_t mdec_out_word;
static void set_option(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}
void psx_devices_service_to_now(void) { dma_advance(1); }
void psx_advance_cycles_slow(uint32_t n) { psx_cycle_count += n; dma_advance(n); }
void psx_write_word(uint32_t a, uint32_t v) { ram[(a & 0x1ffffc) / 4] = v; }
uint32_t psx_read_word(uint32_t a) { return ram[(a & 0x1ffffc) / 4]; }
void psx_irq_raise(uint32_t b, uint32_t d) { (void)d; i_stat |= 1u << b; irqs++; }
void event_ring_record_aux(uint16_t k, uint8_t s, uint32_t v) { (void)k; (void)s; (void)v; }
int cdrom_dma_ready(void) { return 1; }
uint32_t cdrom_dma_sector_word_count(void) { return 512; }
uint32_t cdrom_dma_read(void) { return 0xCD000000u + cd_reads++; }
uint32_t cdrom_dma_read_padded(void) { return cdrom_dma_read(); }
void cdrom_debug_snapshot(CDROMDebugState *s) { memset(s, 0, sizeof *s); }
int cdrom_get_setloc_lba(void) { return 4; }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
void overlay_capture_before_dma(uint32_t a, uint32_t n) { (void)a; (void)n; }
void overlay_capture_on_dma(uint32_t a, uint32_t n, const uint8_t *p) { (void)a; (void)n; (void)p; }
void dirty_ram_mark_executable_range(uint32_t a, uint32_t n) { (void)a; (void)n; }
void psx_fatal_halt(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
int mdec_source_active(void) { return mdec_on; }
void mdec_source_advance(uint32_t n) { (void)n; }
uint32_t mdec_source_dma_read(uint32_t *o) { *o = 0; return mdec_out_word++; }
int mdec_dma_write_ready(void) { return mdec_in_ready; }
int mdec_dma_read_ready(void) { return mdec_out_ready; }
uint32_t mdec_dma_write_words(const uint32_t *p, uint32_t n) { (void)p; (void)n; return 0; }
void mdec_dma_write_word(uint32_t v) { (void)v; }
uint32_t mdec_dma_read_word(void) { return 0; }
void mdec_debug_dma_in_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_in_end(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_end(uint32_t a, uint32_t n) { (void)a; (void)n; }
void gpu_set_gp0_linked_list_node(uint32_t a, uint32_t n) {
    (void)a; (void)n;
    if (++ll_nodes_fetched >= ready_after_nodes) gpu_ready = 0;
}
uint32_t gpu_read_gpuread(void) { return 0x6E000000u + gpuread_next++; }
void gpu_set_gp0_source(uint32_t a) { (void)a; }
void gpu_write_gp0(uint32_t v) {
    gp0_words++; gp0_last = v;
    if (gp0_words >= ready_after_words) gpu_ready = 0;
}
uint32_t gpu_dma_vram_upload_words(void) { return 1u << 20; }
int gpu_dma_source_ll_ready(void) { return gpu_ready; }
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) { (void)a; }
void gpu_ws_restore_linked_list_rank(uint32_t r) { (void)r; }
void gpu_ws_validate_linked_list_header(uint32_t a, uint32_t h) { (void)a; (void)h; }
void gpu_ws_validate_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; }
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) { return a; }
void spu_dma_write(uint32_t v) { spu_in = spu_in * 33u + v; }
uint32_t spu_dma_read(void) { return 0x5B000000u + spu_reads++; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
int source_gpu_runtime_active(void) { return 0; }
uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }
void source_gpu_runtime_copy(SourceGPUServiceClock *c, SourceGPUCommandProjection *p) { (void)c; (void)p; abort(); }
void source_gpu_runtime_dma_write(void) {}

#define REG(ch, r) (0x1F801080u + 0x10u * (ch) + (r))
#define DPCR 0x1F8010F0u
#define DICR 0x1F8010F4u
static void fresh(uint32_t dpcr_value) {
    psx_cycle_count = 0; psx_next_service_cycle = 0;
    dma_init(); memset(ram, 0, sizeof ram);
    gp0_words = gp0_last = gpuread_next = spu_in = spu_reads = cd_reads = irqs = i_stat = 0;
    ll_nodes_fetched = 0; ready_after_nodes = ready_after_words = 0xFFFFFFFFu;
    gpu_ready = 1; mdec_out_ready = mdec_in_ready = 0; mdec_out_word = 0;
    dma_write(DPCR, dpcr_value);
}
/* One guest cycle at a time, as the runtime schedule does. */
static void run(uint32_t n) { while (n--) { psx_cycle_count++; dma_advance(1); } }
static void run_to(uint64_t t) { if (t > psx_cycle_count) run((uint32_t)(t - psx_cycle_count)); }
/* A list of `nodes` nodes of `words` payload words each at 0x10000; the last
 * points at 00FFFFFFh. */
static uint32_t list(uint32_t nodes, uint32_t words, uint32_t fill) {
    uint32_t a = 0x10000;
    for (uint32_t i = 0; i < nodes; i++) {
        uint32_t next = i + 1 < nodes ? a + 4 * (words + 1) : 0xFFFFFF;
        ram[a / 4] = (words << 24) | next;
        for (uint32_t w = 0; w < words; w++) ram[a / 4 + 1 + w] = fill;
        a += 4 * (words + 1);
    }
    return 0x10000;
}
static void kick(int ch, uint32_t madr, uint32_t bcr, uint32_t chcr) {
    dma_write(REG(ch, 0), madr); dma_write(REG(ch, 4), bcr); dma_write(REG(ch, 8), chcr);
}
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

/* Spec 7.5, case 4 (Tekken 3): a stop while the list waits between nodes. */
static int stop_between_nodes(void) {
    fresh(0x0FEDCBA9u); dma_write(DICR, 0x00840000u);          /* ch2 IRQ enabled */
    list(4, 1, 0x00000000u); ready_after_nodes = 1;            /* GPU busy after node 1 */
    kick(2, 0x10000, 0, 0x01000401);
    CHECK(gp0_words == 1, "stop-between: node 1 payload not sent (%u)", gp0_words);
    run(300);
    dma_write(REG(2, 8), 0x00000401);
    CHECK(dma_read(REG(2, 8)) == 0x00000401u, "stop-between: CHCR2 %08X", dma_read(REG(2, 8)));
    CHECK(dma_read(REG(2, 0)) == 0x10008u, "stop-between: MADR2 %08X, want the next node", dma_read(REG(2, 0)));
    CHECK(!(dicr & (1u << 26)) && !(i_stat & 8u), "stop-between: a flag or IRQ was raised");
    gpu_ready = 1; run(1000);
    CHECK(gp0_words == 1, "stop-between: words moved after the stop");
    return 0;
}

/* Spec 7.5: a stop inside a node sends the rest of that node, no flag. */
static int stop_mid_node(void) {
    fresh(0x0FEDCBA9u); dma_write(DICR, 0x00840000u);
    list(2, 255, 0x00000000u);
    kick(2, 0x10000, 0, 0x01000401);                           /* 64: header 15, 49 words */
    CHECK(gp0_words == 49, "stop-mid: kick moved %u words", gp0_words);
    run(10);
    dma_write(REG(2, 8), 0x00000401);
    CHECK(gp0_words == 255, "stop-mid: %u words, want the whole node (255)", gp0_words);
    CHECK(dma_read(REG(2, 0)) == 0x10000u + 4 * 256, "stop-mid: MADR2 %08X", dma_read(REG(2, 0)));
    CHECK(!(dicr & (1u << 26)) && !(i_stat & 8u), "stop-mid: a flag or IRQ was raised");
    return 0;
}

/* Spec 6.2: DPCR does not gate a kick (D7). */
static int dpcr_not_consulted(void) {
    fresh(0);
    kick(6, 0x1000, 16, 0x11000002);
    CHECK(!(channels[6].chcr & (1u << 24)), "dpcr: OTC did not run with DPCR = 0");
    CHECK(ram[0x1000 / 4] == 0xFFCu && ram[(0x1000 - 15 * 4) / 4] == 0x00FFFFFFu, "dpcr: OT not written");
    return 0;
}

/* Spec 2.4, 5.2: an upload starts a block only when the GPU is ready; the read
 * stall is BS - 1 while it is live and ready, and 0 for BS = 0. */
static int upload_readiness_and_stall(void) {
    fresh(0x0FEDCBA9u);
    gpu_ready = 0;
    kick(2, 0x10000, (20u << 16) | 16, 0x01000201);
    CHECK(gp0_words == 0, "upload: %u words moved while the GPU was not ready", gp0_words);
    CHECK(dma_cpu_read_penalty() == 0, "upload: stall %u while not ready", dma_cpu_read_penalty());
    run(300);
    CHECK(gp0_words == 0, "upload: words moved before the GPU was ready");
    gpu_ready = 1;
    run_to(384);
    CHECK(gp0_words > 0 && dma_cpu_read_penalty() == 15, "upload: words %u stall %u after ready", gp0_words, dma_cpu_read_penalty());
    run_to(1024);
    CHECK(gp0_words == 320 && !(channels[2].chcr & (1u << 24)), "upload: not complete (%u)", gp0_words);
    fresh(0x0FEDCBA9u);
    ready_after_words = 1;                                     /* live, not ready for the next block */
    kick(2, 0x10000, (1u << 16) | 0, 0x01000201);
    CHECK(dma_cpu_read_penalty() == 0, "upload: BS = 0 stall %u, want 0", dma_cpu_read_penalty());
    return 0;
}

/* Spec 1.1 c, 10 (cases 2 and 3): a frame-return service point is exact for
 * the MDEC channels: a DMA1 block that can start after the last edge has its
 * words in RAM at the return. */
static int frame_return_exact(void) {
    mdec_on = 1;
    fresh(0x0FEDCBA9u);
    kick(1, 0x30000, (1u << 16) | 32, 0x01000200);             /* waits: no output yet */
    CHECK(ram[0x30000 / 4] == 0, "frame: a word moved before the MDEC had output");
    run_to(130); mdec_out_ready = 1;
    run_to(200);
    CHECK(ram[0x30000 / 4] == 0, "frame: a word moved between edges without a service point");
    dma_source_gpu_service_at(200);                            /* the frame return */
    int ok = ram[0x30000 / 4 + 31] == 31u && !(channels[1].chcr & (1u << 24));
    mdec_on = 0;
    CHECK(ok, "frame: DMA1 block not in RAM at the return (last word %08X)", ram[0x30000 / 4 + 31]);
    return 0;
}

/* Spec 1.1 b, 1.3 (V2): a register write is a service point for the list: it
 * gains the elapsed cycles at once. 64 at the kick: 7 empty headers (debt 6);
 * a DICR write 40 cycles later: 4 more. */
static int list_exact_at_write(void) {
    fresh(0x0FEDCBA9u);
    list(256, 0, 0);
    kick(2, 0x10000, 0, 0x01000401);
    run(40);
    dma_write(DICR, dicr);
    CHECK(dma_read(REG(2, 0)) == 0x10000u + 4 * 11, "list-write: MADR2 %08X, want %08X", dma_read(REG(2, 0)), 0x10000u + 4 * 11);
    return 0;
}

/* Spec 7.2 (SyncMode 1): a MADR write while the channel waits at a block start
 * steers the next block. */
static int busy_madr_write(void) {
    fresh(0x0FEDCBA9u);
    for (uint32_t i = 0; i < 16; i++) ram[0x20000 / 4 + i] = 0xB0000000u + i;
    ready_after_words = 16;                                    /* GPU busy after block 1 */
    kick(2, 0x10000, (2u << 16) | 16, 0x01000201);
    CHECK(gp0_words == 16, "madr: block 1 moved %u words", gp0_words);
    run(10);
    dma_write(REG(2, 0), 0x20000);
    ready_after_words = 0xFFFFFFFFu; gpu_ready = 1;
    run_to(512);
    CHECK(gp0_words == 32 && gp0_last == 0xB000000Fu, "madr: block 2 did not use the written address (%08X)", gp0_last);
    CHECK(dma_read(REG(2, 0)) == 0x20040u, "madr: MADR2 %08X after block 2", dma_read(REG(2, 0)));
    return 0;
}

/* Spec 6.1, 7.4, 2.4: a CHCR write that keeps bit 24 set does not restart; an
 * OTC waiting for bit 28 starts at the next service point once it is set. */
static int chcr_no_restart(void) {
    fresh(0x0FEDCBA9u);
    ram[0x1000 / 4] = 0xDEAD;
    kick(6, 0x1000, 16, 0x01000002);
    run(50);
    dma_write(REG(6, 8), 0x11000002);
    CHECK(ram[0x1000 / 4] == 0xDEAD, "restart: the OTC ran at the write");
    run_to(256);
    CHECK(ram[0x1000 / 4] == 0xFFCu && !(channels[6].chcr & (1u << 24)), "restart: the OTC did not start at the next edge");
    return 0;
}

/* Spec 9.5: a channel 5 kick only sets bit 24. */
static int channel5_stays_busy(void) {
    fresh(0x0FEDCBA9u); dma_write(DICR, 0x00A00000u);
    kick(5, 0x1000, 4, 0x01000200);
    run(1000);
    CHECK((dma_read(REG(5, 8)) >> 24) & 1u, "ch5: CHCR5 bit 24 cleared");
    CHECK(!(dicr & (1u << 29)) && !irqs, "ch5: a flag or IRQ was raised");
    return 0;
}

/* Spec 7.11: offset +Ch is a second CHCR address. */
static int chcr_mirror(void) {
    fresh(0x0FEDCBA9u);
    dma_write(REG(6, 0), 0x1000); dma_write(REG(6, 4), 16);
    dma_write(REG(6, 0xC), 0x11000002);
    CHECK(ram[0x1000 / 4] == 0xFFCu, "mirror: a write to +Ch did not kick");
    CHECK(dma_read(REG(6, 0xC)) == dma_read(REG(6, 8)), "mirror: +Ch reads %08X", dma_read(REG(6, 0xC)));
    return 0;
}

/* Spec 3.2, 3.6 (V6): a next pointer with bit 23 set but not 00FFFFFFh raises
 * the bus error at the next header fetch. */
static int bus_error(void) {
    fresh(0x0FEDCBA9u); dma_write(DICR, 0x00840000u);
    ram[0x10000 / 4] = (1u << 24) | 0x800000u; ram[0x10004 / 4] = 0;
    kick(2, 0x10000, 0, 0x01000401);
    CHECK(!(channels[2].chcr & (1u << 24)), "bus: CHCR2 still busy");
    CHECK((dicr & (1u << 15)) && !(dicr & (1u << 26)), "bus: DICR %08X", dma_read(DICR));
    CHECK(i_stat & 8u, "bus: no DMA IRQ");
    return 0;
}

/* Spec 3.5, 9.4 (V7): the SPU block count decreases at the block start. */
static int spu_count_at_start(void) {
    fresh(0x0FEDCBA9u);
    kick(4, 0x1000, (4u << 16) | 16, 0x01000201);
    CHECK((channels[4].chcr >> 24) & 1u, "spu: finished at the kick");
    CHECK((dma_read(REG(4, 4)) >> 16) == 3u, "spu: BCR4 %08X mid-block, want count 3", dma_read(REG(4, 4)));
    return 0;
}

/* Spec 6.5: three shapes that must not stop the run. */
static int shapes_accepted(void) {
    fresh(0x0FEDCBA9u);
    kick(2, 0x10000, 128, 0x01000001);                         /* GPU SyncMode 0 burst: halts */
    CHECK(gp0_words == 128 && !(channels[2].chcr & (1u << 24)), "shape: GPU burst %u words", gp0_words);
    fresh(0x0FEDCBA9u);
    kick(2, 0x10000, (2u << 16) | 8, 0x01000200);              /* GPU to RAM, SyncMode 1 */
    run_to(512);
    CHECK(ram[0x10000 / 4] == 0x6E000000u && ram[0x10000 / 4 + 15] == 0x6E00000Fu, "shape: GPUREAD words not in RAM");
    fresh(0x0FEDCBA9u);
    kick(4, 0x1000, 8, 0x01000001);                            /* SPU SyncMode 0: halts */
    CHECK(!(channels[4].chcr & (1u << 24)), "shape: SPU burst not complete");
    return 0;
}

/* Spec 2.3 (channel 2, SyncMode 1): the block-start charge and the block's
 * first word are one step, so that word moves even when the charge alone would
 * exhaust the allowance. Bio return 19,576 samples this exact frontier: the
 * kick at 11,081,809,930 plus 10,636 elapsed cycles gives an allowance of
 * 10,700 = 465 blocks (7+16) + 5, one word short of the frontier word 0x1D852C
 * (PS1B-186). */
static int gpu_block_start_charges_one_step(void) {
    fresh(0x0FEDCBA9u);
    kick(2, 0x10000, (8u << 16) | 16, 0x01000200);   /* GPU to RAM, 8 blocks of 16 */
    CHECK(gpuread_next == 43u, "block start: the kick moved %u words, want 43 (two blocks + 11)", gpuread_next);
    run_to(5);
    dma_source_gpu_service_at_exact(5);              /* +5 credit: block 3 ends, credit 0 */
    CHECK(gpuread_next == 48u, "block start: %u words after block 3, want 48", gpuread_next);
    run_to(7);
    dma_source_gpu_service_at_exact(7);              /* +2 credit: the block-4 charge exceeds it */
    CHECK(gpuread_next == 49u, "block start: %u words, want the charge and the first word in one step", gpuread_next);
    CHECK(ram[(0x10000u + 48u * 4u) / 4] == 0x6E000030u, "block start: the frontier word is not in RAM");
    return 0;
}

/* Spec 2.2: a debt left by the last header is cancelled by the first service
 * point that lifts the allowance above zero, even while the GPU is not ready. */
static int debt_discarded(void) {
    fresh(0x0FEDCBA9u);
    list(256, 0, 0);
    ready_after_nodes = 7;                                     /* kick: 7 headers, debt 6 */
    kick(2, 0x10000, 0, 0x01000401);
    run_to(130); gpu_ready = 1; ready_after_nodes = 0xFFFFFFFFu;
    run(1);
    dma_write(DICR, dicr);                                     /* 3 cycles after edge 128 */
    CHECK(dma_read(REG(2, 0)) == 0x10000u + 4 * 8, "debt: MADR2 %08X, want one more header", dma_read(REG(2, 0)));
    return 0;
}

/* Spec 3.1, 3.5: a decrementing upload (CHCR bit 1) steps the address by -4;
 * MADR takes the address after the block. */
static int upload_reverse(void) {
    fresh(0x0FEDCBA9u);
    for (uint32_t i = 0; i < 64; i++) ram[0x10000 / 4 + i] = 0xC0000000u + i;
    kick(2, 0x10080, (1u << 16) | 16, 0x01000203);             /* 7 + 16 <= 64: whole block */
    CHECK(gp0_words == 16 && gp0_last == 0xC0000000u + 0x20 - 15, "reverse: last word %08X", gp0_last);
    CHECK(dma_read(REG(2, 0)) == 0x10080u - 64, "reverse: MADR2 %08X", dma_read(REG(2, 0)));
    return 0;
}

/* Spec 2.3, 3.4: a chopped SyncMode 0 upload costs 8 cycles a word, and MADR
 * and BCR bits 0-15 track each word. */
static int upload_chopped(void) {
    fresh(0x0FEDCBA9u);
    kick(2, 0x10000, 20, 0x01000101);
    CHECK(gp0_words == 8, "chop: kick moved %u words, want 8 (64 / 8)", gp0_words);
    CHECK(dma_read(REG(2, 0)) == 0x10020u && (dma_read(REG(2, 4)) & 0xFFFFu) == 12u,
          "chop: MADR2 %08X BCR2 %08X after the kick", dma_read(REG(2, 0)), dma_read(REG(2, 4)));
    run_to(128);
    CHECK(gp0_words == 20 && dma_read(REG(2, 0)) == 0x10050u && (dma_read(REG(2, 4)) & 0xFFFFu) == 0u &&
          !(channels[2].chcr & (1u << 24)), "chop: at the edge words %u MADR2 %08X", gp0_words, dma_read(REG(2, 0)));
    return 0;
}

/* Spec 1.8, 13 (D12 a, c, d): a DICR or DPCR rewrite, or a DMA5 MADR write,
 * after an upload kick splits the allowance but never moves the completion,
 * at every kick phase, for the 12x16 and 4x64 uploads. The completion cycle
 * is compared with the run without the write (kick cycle matched). */
static uint64_t upload_done(uint32_t phase, uint32_t ba, uint32_t bs, int x, uint32_t d) {
    fresh(0x0FEDCBA9u);
    run(phase);
    kick(2, 0x10000, (ba << 16) | bs, 0x01000201);
    uint64_t k = psx_cycle_count;
    for (uint32_t t = 0; t < 4000; t++) {
        if (t == d) {
            if (x == 'a') dma_write(DICR, dicr & 0x00FFFFFFu);
            else if (x == 'c') dma_write(DPCR, dpcr);
            else if (x == 'd') dma_write(REG(5, 0), 0);
        }
        if (!(channels[2].chcr & (1u << 24))) return psx_cycle_count - k;
        run(1);
    }
    return 0;
}
static int d12_kick_matched(void) {
    static const uint32_t shapes[2][2] = { { 12, 16 }, { 4, 64 } };
    static const uint32_t ds[5] = { 20, 40, 60, 90, 110 };
    for (unsigned s = 0; s < 2; s++)
        for (uint32_t phase = 0; phase < 128; phase++) {
            uint64_t f = upload_done(phase, shapes[s][0], shapes[s][1], 'f', 0);
            CHECK(f, "d12: %ux%u phase %u never completed", shapes[s][0], shapes[s][1], phase);
            for (unsigned i = 0; i < 5; i++)
                for (const char *x = "acd"; *x; x++) {
                    uint64_t done = upload_done(phase, shapes[s][0], shapes[s][1], *x, ds[i]);
                    CHECK(done == f, "d12 %c: %ux%u phase %u d %u: done %llu, f %llu", *x, shapes[s][0], shapes[s][1],
                          phase, ds[i], (unsigned long long)done, (unsigned long long)f);
                }
        }
    return 0;
}

int main(int argc, char **argv) {
    set_option("PSX_INPUT_ROUTE_FILE", "authored-fixture");
    set_option("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad");
    set_option("PSX_INPUT_ROUTE_DMA_MODEL", "octoshock-2.2.2-otc");
    set_option("PSX_CD_DMA_MODEL", "octoshock-2.2.2");
    static const struct { const char *name; int (*fn)(void); } cases[] = {
        { "7.5 stop between nodes", stop_between_nodes },
        { "7.5 stop mid-node", stop_mid_node },
        { "6.2 DPCR", dpcr_not_consulted },
        { "2.4/5.2 upload readiness and stall", upload_readiness_and_stall },
        { "1.1c frame return", frame_return_exact },
        { "1.1b list at a write", list_exact_at_write },
        { "7.2 busy MADR", busy_madr_write },
        { "7.4 no restart", chcr_no_restart },
        { "9.5 channel 5", channel5_stays_busy },
        { "7.11 CHCR mirror", chcr_mirror },
        { "3.2 bus error", bus_error },
        { "3.5 SPU count", spu_count_at_start },
        { "6.5 shapes", shapes_accepted },
        { "2.3 GPU block start in one step", gpu_block_start_charges_one_step },
        { "2.2 debt", debt_discarded },
        { "1.8/13 D12 kick-matched", d12_kick_matched },
        { "3.1 reverse upload", upload_reverse },
        { "3.4 chopped upload", upload_chopped },
    };
    int failed = 0;
    if (argc == 2) {                                           /* one case, by index */
        unsigned i = (unsigned)atoi(argv[1]);
        if (i >= sizeof cases / sizeof cases[0]) return 2;
        return cases[i].fn();
    }
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (cases[i].fn()) { fprintf(stderr, "FAIL spec %s\n", cases[i].name); failed++; }
    if (failed) return 1;
    puts("PASS spec v2: stop, DPCR, upload readiness/stall, frame return, list at a write, busy MADR, no restart, channel 5, CHCR mirror, bus error, SPU count, shapes, debt, D12 kick-matched, reverse and chopped uploads");
    return 0;
}
