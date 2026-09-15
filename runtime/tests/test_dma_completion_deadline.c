#define PSX_TEST_REAL_CYCLE_SCHEDULER
/* Authored production-controller fixture. No BIOS, disc or retail code. */
#define _POSIX_C_SOURCE 200809L
#include "../src/dma.c"
#include <assert.h>
uint64_t s_frame_count;
int g_ls_replay_active;
#ifndef PSX_TEST_REAL_CYCLE_SCHEDULER
uint64_t psx_cycle_count,psx_next_service_cycle;
int psx_in_device_service,g_event_step_conservative;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
#endif
uint32_t i_stat,g_debug_current_func_addr,g_debug_last_store_pc;
static uint32_t ram[0x80000],writes,irqs,read_words,available,gpu_words,gpu_headers;
static void set_option(const char *key,const char *value) {
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
}
#ifndef PSX_TEST_REAL_CYCLE_SCHEDULER
void psx_devices_service_to_now(void) {
#ifdef PSX_TEST_SOURCE_CD_IMPLEMENTED
    advance_source_cdrom();
#endif
}
void psx_advance_cycles_slow(uint32_t n) {psx_cycle_count+=n;psx_devices_service_to_now();}
#endif
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
int mdec_source_active(void) {return 0;}
void mdec_source_advance(uint32_t n) {(void)n;abort();}
uint32_t mdec_source_dma_read(uint32_t *o) {(void)o;abort();}
int mdec_dma_write_ready(void) {abort();}
int mdec_dma_read_ready(void) {abort();}
uint32_t mdec_dma_write_words(const uint32_t *p,uint32_t n) {(void)p;(void)n;abort();}
void mdec_dma_write_word(uint32_t v) {(void)v;abort();}
uint32_t mdec_dma_read_word(void) {abort();}
#define UNUSED_DMA_STUB(name) void name(uint32_t a,uint32_t n) {(void)a;(void)n;abort();}
UNUSED_DMA_STUB(mdec_debug_dma_in_start)
UNUSED_DMA_STUB(mdec_debug_dma_out_start)
UNUSED_DMA_STUB(mdec_debug_dma_in_end)
UNUSED_DMA_STUB(mdec_debug_dma_out_end)
void gpu_set_gp0_linked_list_node(uint32_t a,uint32_t n){(void)a;(void)n;gpu_headers++;}
uint32_t gpu_read_gpuread(void) {abort();}
uint32_t gpu_dma_vram_upload_words(void) {abort();}
void gpu_set_gp0_source(uint32_t a) {(void)a;}
void gpu_write_gp0(uint32_t v) {(void)v;gpu_words++;}
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) {(void)a;}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) {return a&0x1ffffcu;}
void spu_dma_write(uint32_t v) {(void)v;abort();}
uint32_t spu_dma_read(void) {abort();}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;abort();}
#include "../src/psx_cycles.c"

static uint64_t timer_due, timer_fired_cycle, dma_irq_cycle;
static unsigned timer_events;
uint32_t i_mask;
uint32_t interrupts_cycles_to_vblank(void) { return 1000000; }
uint32_t timers_cycles_to_irq(uint32_t mask) {
    (void)mask;
    return timer_due && !timer_events ? (uint32_t)(timer_due-psx_cycle_count) : 1000000;
}
uint32_t cdrom_cycles_to_irq(uint32_t mask) { (void)mask; return 1000000; }
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return 1000000; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return 1000000; }
void psx_spu_sample_event_service(void) {}
void interrupts_service_scheduled_events(void) {}
void sio_advance(uint32_t cycles) { (void)cycles; }
void cdrom_advance(uint32_t cycles) { (void)cycles; }
void timers_advance(uint32_t cycles) {
    (void)cycles;
    if (timer_due && !timer_events && psx_cycle_count>=timer_due) {
        timer_events++;timer_fired_cycle=psx_cycle_count;
    }
    if (irqs && !dma_irq_cycle) dma_irq_cycle=psx_cycle_count;
}
void interrupts_advance_cycles(uint32_t cycles) { (void)cycles; }
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
int psx_netplay_active(void) { return 0; }
int psx_selfcheck_enabled(void) { return 0; }
int psx_get_in_exception(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}
uint64_t g_guest_store_count, g_mmio_access_count;
int g_ls_mode, g_precise_mode, g_psx_call_bail;


/* This test validates service of the existing advertised completion deadline.
 * It does not validate the legacy eager GP0 payload or one-cycle/word model. */
static int run_case(uint64_t start,unsigned words,unsigned future,int empty,int masked) {
    dma_init();memset(ram,0,sizeof(ram));irqs=i_stat=0;gpu_words=gpu_headers=0;
    psx_cycle_count=start;s_devices_synced_cycle=start;
    s_next_watchdog=s_next_pc_sample=UINT64_MAX;
    psx_in_device_service=0;g_psx_cycle_fast_limit=0;g_psx_cyc_batch=0;
    g_psx_cyc_batch_limit=0;g_psx_cyc_local_acc=NULL;
    timer_due=future?start+future:0;timer_events=0;timer_fired_cycle=dma_irq_cycle=0;
    i_mask=masked?0:8;dpcr|=8u<<8;dicr=(1u<<23)|(1u<<18);
    ram[0x1000/4]=((words-1)<<24)|0x00ffffff;
    channels[2].madr=0x1000;channels[2].chcr=0;
    psx_devices_mmio_sync();
    if(empty)psx_next_service_cycle=0; /* control: cache already invalid */
    uint64_t cached=psx_next_service_cycle;
    dma_write(0x1f8010a8,0x01000401);
    uint32_t advertised=dma_cycles_to_internal_event();
    assert(advertised==words && psx_cycle_count==start && !irqs);
    assert(gpu_words==words-1 && gpu_headers==1);
    if(words>1)psx_advance_cycles(words-1);
    assert(!irqs && (channels[2].chcr&(1u<<24)));
    psx_advance_cycles(1);
    unsigned timely=irqs==1 && !(channels[2].chcr&(1u<<24));
    printf("start=%llu words=%u future=%u empty=%d masked=%d cached=%llu now=%llu irq=%u busy=%u timely=%u\n",
        (unsigned long long)start,words,future,empty,masked,(unsigned long long)cached,
        (unsigned long long)psx_cycle_count,irqs,(channels[2].chcr>>24)&1,timely);
    if(!timely){psx_devices_service_to_now();assert(irqs==1);return 1;}
    if(timer_due && timer_due<=start+words)assert(timer_events==1 && timer_fired_cycle==timer_due);
    else assert(!timer_events);
    psx_advance_cycles(100);psx_devices_service_to_now();assert(irqs==1 && gpu_words==words-1);
    return 0;
}
int main(void) {
    set_option("PSX_GPU_DMA_MODEL","");set_option("PSX_CD_DMA_MODEL","");set_option("PSX_DMA_MODEL","");
    unsigned failures=0,cases=0;
    uint64_t starts[]={0,123,54337406};unsigned counts[]={1,7,17},futures[]={0,2,7,100};
    for(unsigned s=0;s<3;s++)for(unsigned w=0;w<3;w++)for(unsigned t=0;t<4;t++)
      for(int e=0;e<2;e++)for(int m=0;m<2;m++){cases++;failures+=run_case(starts[s],counts[w],futures[t],e,m);}
    printf("RESULT cases=%u failures=%u\n",cases,failures);return failures?1:0;
}

/* LL option is off in this adjacent fixture. */
int gpu_dma_source_ll_ready(void) {abort();}

/* Source GPU projection is inactive in this isolated controller fixture. */
int source_gpu_runtime_active(void) {return 0;}
void source_gpu_runtime_dma_write(void) {}

/* No source GPU consumer in this device-isolation control. */
void source_gpu_runtime_advance(void) {}
uint32_t source_gpu_runtime_cycles_to_event(void) {return UINT32_MAX;}
