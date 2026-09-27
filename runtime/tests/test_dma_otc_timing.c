/* Synthetic controller test: no BIOS, retail code, or disc is needed.
 * Source-mode values [ORACLE FIXTURE D9a] (S-D9-grain.tsv sha 3d7a4c0e): n <= 64
 * words finish at the kick, longer tables finish at the first 128-cycle service
 * edge that covers n - 64 cycles, and the CPU waits for the whole transfer.
 * Include the implementation to isolate channel 6 from unrelated devices. */
#include "../src/dma.c"
#include <assert.h>
uint64_t s_frame_count;
uint64_t psx_cycle_count, psx_next_service_cycle;
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
static uint32_t ram[0x80000], writes, irqs;
void psx_devices_service_to_now(void) { dsm_service_edges(psx_cycle_count); }
void psx_advance_cycles_slow(uint32_t n) { psx_cycle_count+=n; dsm_service_edges(psx_cycle_count); }
void psx_write_word(uint32_t addr, uint32_t value) { ram[(addr & 0x1ffffc)/4]=value; writes++; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat|=1u<<bit; irqs++; }
void event_ring_record_aux(uint16_t kind,uint8_t src,uint32_t value) { (void)kind; (void)src; (void)value; }
/* The source MDEC is off in this OTC-only fixture. */
int mdec_source_active(void) { return 0; }
void mdec_source_advance(uint32_t n) { (void)n; abort(); }
static void setup(uint32_t count, uint64_t phase) {
    psx_cycle_count=phase; psx_next_service_cycle=0;
    dma_init();
    memset(ram,0xCC,sizeof(ram)); writes=irqs=i_stat=0;
    channels[6].madr=0x100000; channels[6].bcr=count; channels[6].chcr=0x11000002;
    dpcr|=8u<<24;
    dicr=(1u<<23)|(1u<<22);
}
static void tick(uint32_t cycles) { psx_cycle_count+=cycles; dsm_service_edges(psx_cycle_count); }
int main(void) {
    setup(1024,0);
    execute_ch6_otc();
    assert(writes==1024 && psx_cycle_count==0 && !(channels[6].chcr&(1u<<24)));
    for(uint32_t phase=0;phase<128;phase++) {
        setup(1024,phase); dsm_kick(DSM_OTC);
        assert(writes==64 && dsm[DSM_OTC].words_left==960 && irqs==0);
        assert(ram[0x100000/4]==0xFFFFC && ram[(0x100000-64*4)/4]==0xCCCCCCCC);
        uint64_t expected=((phase+960+127)/128)*128;
        tick((uint32_t)(expected-psx_cycle_count)-1);
        assert(dsm[DSM_OTC].words_left && (channels[6].chcr&(1u<<24)) && irqs==0);
        tick(1); assert(writes==1024 && !dsm[DSM_OTC].words_left && irqs==1);
        assert(ram[(0x100000-1023*4)/4]==0xFFFFFF);
        tick(1000); assert(writes==1024 && irqs==1);
    }
    for(uint32_t n=1;n<=65;n++) {
        setup(n,127); dsm_kick(DSM_OTC);
        assert(writes==(n<64?n:64));
        assert((dsm[DSM_OTC].words_left==0)==(n<=64));
        if(n==65) { tick(1); assert(writes==65 && irqs==1); }
    }
    setup(0,0); dsm_kick(DSM_OTC); tick(65536);
    assert(writes==65536 && !dsm[DSM_OTC].words_left && irqs==1);
    setup(1024,43); otc_source_model=1; try_execute(6);
    assert(writes==1024 && irqs==1 && psx_cycle_count==1024);
    assert(dma_snapshot_read(NULL,0)==0); /* source model rejects restore before reading */
    puts("PASS: default, source start budget, all 128 phases, partial RAM, completion/IRQ, short/zero counts, CPU wait, restore guard");
    return 0;
}

/* Source GPU projection is inactive in this isolated controller fixture. */
int source_gpu_runtime_active(void) {return 0;}
void source_gpu_runtime_dma_write(void) {}

/* Devices other than the OTC must never run in this OTC-only fixture; the
 * generic source machine links them. */
void cdrom_debug_snapshot(CDROMDebugState *s) { (void)s; abort(); }
uint32_t cdrom_dma_read_padded(void) { abort(); }
uint32_t cdrom_dma_sector_word_count(void) { abort(); }
int cdrom_get_setloc_lba(void) { abort(); }
void dirty_ram_mark_executable_range(uint32_t a, uint32_t n) { (void)a; (void)n; abort(); }
int gpu_dma_source_ll_ready(void) { abort(); }
uint32_t gpu_dma_vram_upload_words(void) { abort(); }
uint32_t gpu_read_gpuread(void) { abort(); }
void gpu_set_gp0_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; abort(); }
void gpu_set_gp0_source(uint32_t a) { (void)a; abort(); }
void gpu_write_gp0(uint32_t v) { (void)v; abort(); }
int mdec_dma_read_ready(void) { abort(); }
int mdec_dma_write_ready(void) { abort(); }
void mdec_dma_write_word(uint32_t v) { (void)v; abort(); }
uint32_t mdec_source_dma_read(uint32_t *o) { (void)o; abort(); }
uint8_t *memory_get_ram_ptr(void) { abort(); }
void overlay_capture_on_dma(uint32_t a, uint32_t n, const uint8_t *p) { (void)a; (void)n; (void)p; abort(); }
uint32_t psx_read_word(uint32_t a) { (void)a; abort(); }
uint32_t spu_dma_read(void) { abort(); }
void spu_dma_write(uint32_t v) { (void)v; abort(); }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; abort(); }
