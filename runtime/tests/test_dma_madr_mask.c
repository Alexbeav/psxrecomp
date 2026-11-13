/* DMA MADR width (PS1G-159): bits 24-31 of every D#_MADR are zero. Pin I's
 * composed register write lost the mask, and Incredible Crisis's title list
 * walk then never met its 24-bit cursor. Authored; no BIOS or disc. */
#define _POSIX_C_SOURCE 200809L
#include "../src/dma.c"
#include <assert.h>
uint64_t s_frame_count, psx_cycle_count, psx_next_service_cycle;
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t g_io_openbus_reads, g_io_openbus_writes;
static uint32_t ram[0x80000], gp0_words, gp0_sum, spu_in, spu_out, cd_reads, irqs;
static int ll_ready = 1;
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
int mdec_source_active(void) { return 0; }
void mdec_source_advance(uint32_t n) { (void)n; }
uint32_t mdec_source_dma_read(uint32_t *o) { *o = 0; return 0; }
int mdec_dma_write_ready(void) { return 0; }
int mdec_dma_read_ready(void) { return 0; }
uint32_t mdec_dma_write_words(const uint32_t *p, uint32_t n) { (void)p; (void)n; return 0; }
void mdec_dma_write_word(uint32_t v) { (void)v; }
uint32_t mdec_dma_read_word(void) { return 0; }
void mdec_debug_dma_in_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_in_end(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_end(uint32_t a, uint32_t n) { (void)a; (void)n; }
void gpu_set_gp0_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; }
uint32_t gpu_read_gpuread(void) { return 0; }
void gpu_set_gp0_source(uint32_t a) { (void)a; }
void gpu_write_gp0(uint32_t v) { gp0_words++; gp0_sum = gp0_sum * 31u + v; }
uint32_t gpu_dma_vram_upload_words(void) { return 1u << 20; }
int gpu_dma_source_ll_ready(void) { return ll_ready; }
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) { (void)a; }
void gpu_ws_restore_linked_list_rank(uint32_t r) { (void)r; }
void gpu_ws_validate_linked_list_header(uint32_t a, uint32_t h) { (void)a; (void)h; }
void gpu_ws_validate_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; }
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) { return a; }
void spu_dma_write(uint32_t v) { spu_in = spu_in * 33u + v; }
uint32_t spu_dma_read(void) { return spu_out++; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
int source_gpu_runtime_active(void) { return 0; }
uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }
void source_gpu_runtime_copy(SourceGPUServiceClock *c, SourceGPUCommandProjection *p) { (void)c; (void)p; abort(); }
void source_gpu_runtime_dma_write(void) {}


static void kick(uint32_t base, uint32_t madr, uint32_t bcr, uint32_t chcr) {
    dma_write(base, madr); dma_write(base + 4, bcr); dma_write(base + 8, chcr);
}
static void fresh(void) {
    psx_cycle_count = 0; psx_next_service_cycle = 0;
    dma_init(); memset(ram, 0, sizeof ram);
    dma_write(0x1F8010F0, 0x0FEDCBA9u);
}
int main(void) {
    /* [DOC] PSX-SPX "D#_MADR": bits 24-31 are not used (always zero), on every
     * channel. A GPU list cursor is a 24-bit address, and the restart and
     * cancel rules compare it with MADR; an unmasked MADR never matches. */
    fresh();
    for (int ch = 0; ch < 7; ch++) {
        uint32_t base = 0x1F801080u + 0x10u * (uint32_t)ch;
        dma_write(base, 0xFFFFFFFFu);
        assert(channels[ch].madr == 0x00FFFFFFu);
        assert(dma_read(base) == 0x00FFFFFFu);
    }
    dma_write(0x1F8010A0, 0x80001000u);
    assert(channels[2].madr == 0x001000u && dma_read(0x1F8010A0) == 0x001000u);
    /* [EMU] spec 3.2: a list kicked at the 00FFFFFFh terminator is a bus error.
     * No header is read (the top of RAM holds a cyclic empty node here), busy
     * clears, DICR bit 15 sets. */
    fresh(); ram[0x1FFFFC / 4] = 0x001FFFFCu;
    uint32_t words = gp0_words;
    kick(0x1F8010A0, 0x00FFFFFFu, 0, 0x01000401);
    for (int i = 0; i < 4096; i++) { psx_cycle_count++; dma_advance(1); }
    assert(!gpu_linked_list.active);
    assert(!((channels[2].chcr >> 24) & 1u));
    assert((dicr >> 15) & 1u);
    assert(gp0_words == words);
    puts("PASS MADR bits 24-31 read zero on ch0-6; list kicked at the terminator is a bus error");
    return 0;
}

#include "gpu_queue_test_stubs.h"

/* No render pass/frozen guest clock in this isolated device fixture. */
int g_psx_guest_time_frozen, g_psx_render_pass_active;
uint32_t g_psx_ram_mask=0x1fffffu;
int psx_get_in_exception(void) { return 0; }
void gpu_timeline_note(uint8_t kind,uint32_t a,uint32_t b) {(void)kind;(void)a;(void)b;}
