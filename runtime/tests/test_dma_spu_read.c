/* PS1B-454 part B row 6: the SPU-to-RAM direction of DMA channel 4, through
 * the production dma.c and spu.c together.
 *
 * Gran Turismo's race loading (FAIL-027) reads SPU RAM back by DMA. The
 * channel 4 read once zero-filled RAM instead of moving the SPU words, and a
 * title that carries a checksummed block through SPU RAM across an Exec then
 * fails its own check. test_spu_fidelity.c calls spu_dma_read() directly and
 * the DMA tests stub it, so no test ran the read through the controller.
 *
 * Rows:
 *   D1  A channel 4 block transfer from RAM writes the words into SPU RAM at
 *       the SPU transfer address and completes (control for D2).
 *   D2  A channel 4 block transfer to RAM writes the SPU words to RAM at MADR,
 *       in order, and completes: CHCR bit 24 clears and MADR has advanced.
 *   D3  A block written by DMA and read back by DMA is equal.
 *
 * Authored; no BIOS or disc. Default DMA path. */
#define _POSIX_C_SOURCE 200809L
#include "../src/dma.c"
#include <stdbool.h>
#include "spu.h"
uint64_t s_frame_count, psx_cycle_count, psx_next_service_cycle;
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t g_io_openbus_reads, g_io_openbus_writes;
static uint32_t ram[0x80000], irqs;
static int failures;
void psx_devices_service_to_now(void) { dma_advance(1); }
void psx_advance_cycles_slow(uint32_t n) { psx_cycle_count += n; dma_advance(n); }
void psx_write_word(uint32_t a, uint32_t v) { ram[(a & 0x1ffffc) / 4] = v; }
uint32_t psx_read_word(uint32_t a) { return ram[(a & 0x1ffffc) / 4]; }
void psx_irq_raise(uint32_t b, uint32_t d) { (void)d; i_stat |= 1u << b; irqs++; }
void event_ring_record_aux(uint16_t k, uint8_t s, uint32_t v) { (void)k; (void)s; (void)v; }
int cdrom_dma_ready(void) { return 0; }
uint32_t cdrom_dma_sector_word_count(void) { return 0; }
uint32_t cdrom_dma_read(void) { abort(); }
uint32_t cdrom_dma_read_padded(void) { abort(); }
void cdrom_debug_snapshot(CDROMDebugState *s) { memset(s, 0, sizeof *s); }
int cdrom_get_setloc_lba(void) { return 0; }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
void overlay_capture_before_dma(uint32_t a, uint32_t n) { (void)a; (void)n; }
void overlay_capture_on_dma(uint32_t a, uint32_t n, const uint8_t *p) { (void)a; (void)n; (void)p; }
void dirty_ram_mark_executable_range(uint32_t a, uint32_t n) { (void)a; (void)n; }
void psx_fatal_halt(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
int mdec_source_active(void) { return 0; }
void mdec_source_advance(uint32_t n) { (void)n; }
uint32_t mdec_source_dma_read(uint32_t *o) { (void)o; abort(); }
int mdec_dma_write_ready(void) { return 0; }
int mdec_dma_read_ready(void) { return 0; }
uint32_t mdec_dma_write_words(const uint32_t *p, uint32_t n) { (void)p; (void)n; abort(); }
void mdec_dma_write_word(uint32_t v) { (void)v; abort(); }
uint32_t mdec_dma_read_word(void) { abort(); }
void mdec_debug_dma_in_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_in_end(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_end(uint32_t a, uint32_t n) { (void)a; (void)n; }
void gpu_set_gp0_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; }
uint32_t gpu_read_gpuread(void) { abort(); }
void gpu_set_gp0_source(uint32_t a) { (void)a; }
void gpu_write_gp0(uint32_t v) { (void)v; abort(); }
uint32_t gpu_dma_vram_upload_words(void) { abort(); }
int gpu_dma_source_ll_ready(void) { return 1; }
int gpu_dma_walk_ready(void) { return 1; }
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) { (void)a; }
void gpu_ws_restore_linked_list_rank(uint32_t r) { (void)r; }
void gpu_ws_validate_linked_list_header(uint32_t a, uint32_t h) { (void)a; (void)h; }
void gpu_ws_validate_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; }
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) { return a; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
int source_gpu_runtime_active(void) { return 0; }
uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }
void source_gpu_runtime_copy(SourceGPUServiceClock *c, SourceGPUCommandProjection *p) { (void)c; (void)p; abort(); }
void source_gpu_runtime_dma_write(void) {}
/* What spu.c needs from the rest of the runtime. */
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void audio_trace_pcm(int tap, const int16_t *stereo, int frames) { (void)tap; (void)stereo; (void)frames; }
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) { (void)canon; (void)frames; }

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)
#define WORDS 64u          /* four blocks of sixteen words */
#define SPU_BYTE 0x1000u   /* SPU RAM byte address of the block */
#define R_XFERADDR 0x1F801DA6u
#define R_SPUCNT 0x1F801DAAu

static uint32_t pattern(uint32_t i) { return 0x9E3779B9u * (i + 1u) ^ (i << 8); }
static void kick(uint32_t madr, uint32_t chcr) {
    dma_write(0x1F8010C0, madr); dma_write(0x1F8010C4, (4u << 16) | 16u); dma_write(0x1F8010C8, chcr);
}
/* Runs until CHCR bit 24 clears; returns 1 when it did. */
static int run_to_completion(void) {
    for (int i = 0; i < 200000; i++) {
        if (!((channels[4].chcr >> 24) & 1u)) return 1;
        psx_cycle_count++; dma_advance(1);
    }
    return !((channels[4].chcr >> 24) & 1u);
}

int main(void) {
    char what[160];
    psx_cycle_count = 0; psx_next_service_cycle = 0;
    dma_init(); memset(ram, 0, sizeof ram); spu_init();
    dma_write(0x1F8010F0, 0x0FEDCBA9u);                 /* every channel enabled */

    /* D1: RAM -> SPU. */
    for (uint32_t i = 0; i < WORDS; i++) ram[0x10000 / 4 + i] = pattern(i);
    spu_write(R_XFERADDR, SPU_BYTE / 8u);
    spu_write(R_SPUCNT, 0x8020u);                        /* SPU on, DMA write */
    kick(0x10000, 0x01000201u);                          /* from RAM, SyncMode 1 */
    CHECK(run_to_completion(), "D1: the RAM-to-SPU transfer completes");
    const uint8_t *spu = spu_get_ram();
    int written = 1;
    for (uint32_t i = 0; i < WORDS; i++) {
        uint32_t w = (uint32_t)spu[SPU_BYTE + 4 * i] | (uint32_t)spu[SPU_BYTE + 4 * i + 1] << 8 |
                     (uint32_t)spu[SPU_BYTE + 4 * i + 2] << 16 | (uint32_t)spu[SPU_BYTE + 4 * i + 3] << 24;
        if (w != pattern(i)) written = 0;
    }
    CHECK(written, "D1: SPU RAM holds the words at the SPU transfer address");

    /* D2, D3: SPU -> RAM, into a cleared area. */
    memset(&ram[0x20000 / 4], 0, WORDS * 4u);
    spu_write(R_XFERADDR, SPU_BYTE / 8u);
    spu_write(R_SPUCNT, 0x8030u);                        /* SPU on, DMA read */
    kick(0x20000, 0x01000200u);                          /* to RAM, SyncMode 1 */
    CHECK(run_to_completion(), "D2: the SPU-to-RAM transfer completes");
    snprintf(what, sizeof what, "D2: MADR advances past the block (got %06X)",
             (unsigned)(channels[4].madr & 0x1FFFFCu));
    CHECK((channels[4].madr & 0x1FFFFCu) == 0x20000u + WORDS * 4u, what);
    uint32_t equal = 0, zero = 0;
    for (uint32_t i = 0; i < WORDS; i++) {
        if (ram[0x20000 / 4 + i] == pattern(i)) equal++;
        if (ram[0x20000 / 4 + i] == 0) zero++;
    }
    snprintf(what, sizeof what, "D2/D3: RAM at MADR holds the SPU words in order (%u of %u equal, %u zero)",
             (unsigned)equal, (unsigned)WORDS, (unsigned)zero);
    CHECK(equal == WORDS, what);

    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: DMA channel 4 reads SPU RAM back into RAM at MADR");
    return 0;
}

#include "gpu_queue_test_stubs.h"
