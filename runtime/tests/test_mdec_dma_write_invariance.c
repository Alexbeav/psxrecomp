/* [ORACLE FIXTURE D18d] register writes do not clock the source MDEC. Replays the
 * D18 group a/d shape through the runtime schedule (dma_advance every cycle) with
 * the source GPU runtime active, so every DMA register write also reaches
 * dma_source_gpu_service_at through the GPU WRITE event: DMA0 kick, DMA1 kick (at
 * once, or 1000 cycles later), with and without one access mid-decode (DMA5
 * MADR/BCR/CHCR, a DICR rewrite, or a bare GPU WRITE event), at all 128 kick
 * phases. The per-cycle MADR1/CHCR and MDEC status timelines must be identical.
 * Authored stream; no BIOS or disc. */
#define _POSIX_C_SOURCE 200809L
#include "dma_gpu_ll.c"
#include "dma.c"
#include "mdec.c"
#include <assert.h>
uint64_t s_frame_count,psx_cycle_count,psx_next_service_cycle;
int psx_in_device_service,g_event_step_conservative,g_ls_replay_active;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
uint32_t i_stat,g_debug_current_func_addr,g_debug_last_store_pc;
static uint32_t ram[0x80000],writes,irqs,read_words,available;
static uint32_t upload_left, uploaded[65536], upload_count;
static void set_option(const char *key,const char *value) {
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
}
void psx_devices_service_to_now(void) {
#ifdef PSX_TEST_SOURCE_GPU_IMPLEMENTED
    advance_source_gpu();
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
    advance_source_gpu_ll();
#endif
#endif
}
void psx_advance_cycles_slow(uint32_t n) {psx_cycle_count+=n;psx_devices_service_to_now();}
void psx_write_word(uint32_t addr,uint32_t value) {ram[(addr&0x1ffffc)/4]=value;writes++;}
void psx_irq_raise(uint32_t bit,uint32_t detail) {(void)detail;i_stat|=1u<<bit;irqs++;}
void event_ring_record_aux(uint16_t kind,uint8_t src,uint32_t value) {(void)kind;(void)src;(void)value;}
int cdrom_dma_ready(void) {return read_words<available;}
uint32_t cdrom_dma_sector_word_count(void) {return available;}
uint32_t cdrom_dma_read(void) {assert(cdrom_dma_ready());return 0xCA000000u+read_words++;}
uint32_t cdrom_dma_read_padded(void) {return cdrom_dma_ready()?cdrom_dma_read():0;}
void cdrom_debug_snapshot(CDROMDebugState *s) {memset(s,0,sizeof(*s));s->last_sector_lba=4;s->sector_size=2048;s->sector_available=1;}
int cdrom_get_setloc_lba(void) {return 4;}
uint8_t *memory_get_ram_ptr(void) {return (uint8_t*)ram;}
void overlay_capture_before_dma(uint32_t a,uint32_t n) {(void)a;(void)n;}
void overlay_capture_on_dma(uint32_t a,uint32_t n,const uint8_t *p) {(void)a;(void)n;(void)p;}
void dirty_ram_mark_executable_range(uint32_t a,uint32_t n) {(void)a;(void)n;}
/* Unrelated device entry points must never execute in this fixture. */
uint64_t g_io_openbus_reads,g_io_openbus_writes;
uint32_t psx_read_word(uint32_t a) {return ram[(a&0x1ffffc)/4];}
void psx_fatal_halt(const char *s) {fprintf(stderr,"%s\n",s);abort();}
void gpu_set_gp0_linked_list_node(uint32_t a,uint32_t b) {(void)a;(void)b;}
uint32_t gpu_read_gpuread(void) {abort();}
void gpu_set_gp0_source(uint32_t a) {(void)a;}
static int ready_state=1;
int gpu_dma_source_ll_ready(void) {return ready_state;}
void gpu_write_gp0(uint32_t v) {uploaded[upload_count++]=v;}
uint32_t gpu_dma_vram_upload_words(void) {return upload_left;}
void gpu_ws_validate_linked_list_header(uint32_t a,uint32_t b) {(void)a;(void)b;abort();}
void gpu_ws_validate_linked_list_node(uint32_t a,uint32_t b) {(void)a;(void)b;abort();}
void gpu_ws_restore_linked_list_rank(uint32_t a) {(void)a;abort();}
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) {(void)a;}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) {return a;}
void spu_dma_write(uint32_t v) {(void)v;abort();}
uint32_t spu_dma_read(void) {abort();}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;abort();}



/* A CPU store to the MDEC, 20 cycles after the previous one (runtime schedule). */
static void mw(uint32_t addr, uint32_t v) { psx_cycle_count += 20; dma_advance(20); mdec_write(addr, v); }
static uint64_t timeline(uint32_t phase, int late_dma1, int write_at, int kind) {
    psx_cycle_count = 0;
    dma_init(); mdec_init(); memset(ram, 0, sizeof ram); i_stat = irqs = 0;
    dma_write(0x1F8010F0, 0x0FEDCBA9u);
    mw(0x1f801824, 0x80000000u); mw(0x1f801824, 0x60000000u);
    mw(0x1f801820, (2u << 29) | 1u);
    for (int i = 0; i < 32; i++) mw(0x1f801820, 0x01010101u);
    mw(0x1f801820, 3u << 29);
    for (int i = 0; i < 32; i++) mw(0x1f801820, 0x5A825A82u);
    for (int b = 0; b < 24; b++) ram[0x40000 / 4 + b] = 0xFE000000u | 0x0408u;   /* DC 8, q_scale 1, then FE00 */
    for (int b = 24; b < 32; b++) ram[0x40000 / 4 + b] = 0xFE00FE00u;
    mw(0x1f801820, (1u << 29) | (2u << 27) | 32u);
    uint64_t t0 = 3000 + phase, h = 1469598103934665603ull;
    for (uint64_t t = psx_cycle_count + 1; t < t0 + 16000; t++) {
        psx_cycle_count = t; dma_advance(1);
        if (t == t0) { dma_write(0x1F801080, 0x40000); dma_write(0x1F801084, (1u << 16) | 32); dma_write(0x1F801088, 0x01000201); }
        uint64_t k1 = t0 + (late_dma1 ? 1000 : 15);
        if (t == k1) { dma_write(0x1F801090, 0x60000); dma_write(0x1F801094, (24u << 16) | 32); dma_write(0x1F801098, 0x01000200); }
        if (write_at && t == t0 + (uint64_t)write_at) {
            if (kind == 0) dma_write(0x1F8010D0, 0);
            else if (kind == 1) dma_write(0x1F8010D4, 0);
            else if (kind == 2) dma_write(0x1F8010D8, 0);
            else if (kind == 3) dma_write(0x1F8010F4, dma_read(0x1F8010F4) & 0x00FFFFFFu);
            else dma_source_gpu_service_at(t);
        }
        if (t >= t0) {
            uint32_t row[4] = { dma_read(0x1f801090), dma_read(0x1f801098), dma_read(0x1f801088), mdec_read(0x1f801824) };
            for (int i = 0; i < 4; i++) h = (h ^ row[i]) * 1099511628211ull;
        }
    }
    return h;
}
int main(void) {
    set_option("PSX_MDEC_SOURCE_MODEL", "octoshock-2.3");
    set_option("PSX_INPUT_ROUTE_FILE", "authored-fixture");
    set_option("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad");
    for (uint32_t phase = 0; phase < 128; phase++) {
        for (int late = 0; late < 2; late++) {
            uint64_t base = timeline(phase, late, 0, 0);
            static const int at[4] = { 300, 332, 364, 396 };
            for (int kind = 0; kind < 5; kind++) for (int i = 0; i < 4; i++)
                if (timeline(phase, late, at[i], kind) != base) {
                    fprintf(stderr, "access kind %d changed the timeline: phase %u late %d at %d\n", kind, phase, late, at[i]);
                    return 1;
                }
        }
    }
    puts("PASS DMA5 MADR/BCR/CHCR, DICR and GPU WRITE-event service leave the source MDEC timeline unchanged at all 128 phases (D18d)");
    return 0;
}

int debug_server_fmv_quiet(void){return 0;}
int source_gpu_runtime_active(void){return 1;}
int source_gpu_runtime_ready(void){return 1;}
uint32_t source_gpu_runtime_cycles_to_event(void){return UINT32_MAX;}
void source_gpu_runtime_dma_write(void){dma_source_gpu_service_at(psx_cycle_count);}
void source_gpu_runtime_copy(SourceGPUServiceClock *c,SourceGPUCommandProjection *s){(void)c;(void)s;abort();}
