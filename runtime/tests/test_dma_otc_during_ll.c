/* A DMA6 (OTC) kick while a ch2 linked list is in flight must not strand ch2
 * (PS1B-186 Tier 1, boot return 96): the list resumes, completes and raises its
 * IRQ, in the source profile and on the default path. Authored; no BIOS or disc. */
#define _POSIX_C_SOURCE 200809L
#include "../src/dma.c"
#include <assert.h>
uint64_t s_frame_count, psx_cycle_count, psx_next_service_cycle;
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
static int gpu_rt; static uint64_t busy_until; static unsigned poly_left;
void gpu_write_gp0(uint32_t v) { gp0_words++; gp0_sum = gp0_sum * 31u + v; if ((v >> 24) == 0x02u) busy_until = psx_cycle_count + 5000;
    /* GPUSTAT.28 as the source projection reports it: 0 once a polygon head is queued, until complete. */
    if (poly_left) poly_left--; else if ((v >> 24) == 0x28u) poly_left = 4; }
uint32_t gpu_dma_vram_upload_words(void) { return 1u << 20; }
int gpu_dma_source_ll_ready(void) { return ll_ready && psx_cycle_count >= busy_until && !poly_left; }
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
int source_gpu_runtime_active(void) { return gpu_rt; }
uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }
void source_gpu_runtime_copy(SourceGPUServiceClock *c, SourceGPUCommandProjection *p) { (void)c; (void)p; abort(); }
void source_gpu_runtime_dma_write(void) { if (gpu_rt) dma_source_gpu_service_at(psx_cycle_count); }


static uint32_t ll_nodes;
/* Runtime-like clock: GPU DMA events every 128 cycles (source GPU service
 * clock), DMA serviced only when dma_cycles_to_internal_event() asks. */
static uint64_t dma_last, dma_next;
static void resched(void) { uint32_t d = dma_cycles_to_internal_event(); dma_next = d == UINT32_MAX ? UINT64_MAX : psx_cycle_count + d; }
static void tick(void) {
    psx_cycle_count++;
    if (gpu_rt && psx_cycle_count % 128 == 0) { dma_source_gpu_service_at(psx_cycle_count); resched(); }
    if (!gpu_rt || psx_cycle_count >= dma_next) {
        dma_advance((uint32_t)(psx_cycle_count - dma_last)); dma_last = psx_cycle_count; resched();
    }
}
/* A list of `nodes` nodes of `words` payload words each at 0x10000, end code last. */
static void build_list(uint32_t nodes, uint32_t words) {
    uint32_t a = 0x10000;
    for (uint32_t i = 0; i < nodes; i++) {
        uint32_t next = i + 1 < nodes ? a + 4 * (words + 1) : 0xFFFFFF;
        ram[a / 4] = (words << 24) | next;
        for (uint32_t w = 0; w < words; w++) ram[a / 4 + 1 + w] = 0x01000000u;   /* GP0 NOPs */
        a += 4 * (words + 1);
    }
    ll_nodes = nodes;
}
static int run(int source, int ready_gap, int fills) {
    set_option("PSX_INPUT_ROUTE_FILE", "authored-fixture");
    set_option("PSX_GPU_DMA_MODEL", source ? "octoshock-2.2.2-bounded-quad" : "");
    set_option("PSX_INPUT_ROUTE_DMA_MODEL", source ? "octoshock-2.2.2-otc" : "");
    dma_init(); memset(ram, 0, sizeof ram); psx_cycle_count = 1000; psx_next_service_cycle = 0;
    i_stat = irqs = 0; gp0_words = 0; ll_ready = 1; busy_until = 0; poly_left = 0; gpu_rt = source; dma_last = psx_cycle_count; dma_next = 0;
    dma_write(0x1F8010F0, 0x0FEDCBA9u); dma_write(0x1F8010F4, (1u << 23) | (1u << 18));
    build_list(fills == 1 ? 16 : 64, fills == 1 ? 3 : fills == 2 ? 6 : 15);
    if (fills == 1) for (uint32_t i = 0; i < 16; i++) ram[0x10000 / 4 + i * 4 + 1] = 0x02000000u;   /* FillRect per node */
    if (fills == 2) for (uint32_t i = 0; i < 64; i++) {   /* E6h then a flat quad (28h, 5 words) per node, as in the Bio list */
        ram[0x10000 / 4 + i * 7 + 1] = 0xE6000000u; ram[0x10000 / 4 + i * 7 + 2] = 0x28808080u;
        for (uint32_t w = 3; w < 7; w++) ram[0x10000 / 4 + i * 7 + w] = 0x00100010u;
    }
    /* The game's order: ch2 list (bit 28 clear), then OTC set up and kicked. */
    dma_write(0x1F8010A0, 0x10000); dma_write(0x1F8010A4, 0); dma_write(0x1F8010A8, 0x01000401);
    resched();
    for (int i = 0; i < 390; i++) { tick(); if (ready_gap && i == 100) ll_ready = 0; }
    if (fills == 1 && source && !((channels[2].chcr >> 24) & 1u)) return 10;                      /* list must still be live */
    dma_write(0x1F8010E8, 0x00000000); dma_write(0x1F8010E0, 0x0EF8B4); dma_write(0x1F8010E4, 0x0400);
    dma_write(0x1F8010E8, 0x11000002); resched();
    while (dma_cpu_source_halted()) tick();                               /* the source CPU owns the halt */
    if (channels[6].chcr != 0x00000002u) return 11;                       /* OTC done */
    if (ram[0x0EF8B4 / 4 - 0x3FF] != 0x00FFFFFFu) return 12;               /* OT end marker */
    for (int i = 0; i < 400000 && ((channels[2].chcr >> 24) & 1u); i++) {
        tick();
        if (ready_gap && i == 2000) ll_ready = 1;
    }
    if ((channels[2].chcr >> 24) & 1u) return 13;                          /* stranded */
    if (!(i_stat & 8u)) return 14;                                          /* no DMA IRQ */
    if (gp0_words != (fills == 1 ? 16u * 3u : fills == 2 ? 64u * 6u : 64u * 15u)) return 15;
    return 0;
}
int main(void) {
    for (int source = 0; source < 2; source++)
        for (int gap = 0; gap < 2; gap++) for (int fills = 0; fills < 3; fills++) {
            int r = run(source, gap, fills);
            if (r) { fprintf(stderr, "OTC during ch2 list: source %d gpu-busy gap %d fills %d failed at %d (CHCR2 %08X)\n",
                             source, gap, fills, r, channels[2].chcr); return 1; }
        }
    puts("PASS ch2 linked list resumes and completes with its IRQ after a mid-list OTC kick (default and source; GPU ready/busy; NOP, fill and E6h+polygon lists)");
    return 0;
}
