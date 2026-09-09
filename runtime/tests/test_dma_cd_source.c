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
static uint32_t ram[0x80000],writes,irqs,read_words,available;
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
UNUSED_DMA_STUB(gpu_set_gp0_linked_list_node)
uint32_t gpu_read_gpuread(void) {abort();}
uint32_t gpu_dma_vram_upload_words(void) {abort();}
void gpu_set_gp0_source(uint32_t a) {(void)a;abort();}
void gpu_write_gp0(uint32_t v) {(void)v;abort();}
void gpu_ws_begin_linked_list(void) {abort();}
void gpu_ws_end_linked_list(void) {abort();}
void gpu_ws_prepass_linked_list(uint32_t a) {(void)a;abort();}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) {(void)a;abort();}
void spu_dma_write(uint32_t v) {(void)v;abort();}
uint32_t spu_dma_read(void) {abort();}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;abort();}
static void setup(uint32_t count,uint64_t phase) {
    dma_init();memset(ram,0xCC,sizeof(ram));writes=irqs=i_stat=read_words=0;
    available=count?count:65536;psx_cycle_count=phase;psx_next_service_cycle=0;
    channels[3].madr=0x10000;channels[3].bcr=count;channels[3].chcr=0x11000000;
    dpcr|=8u<<12;dicr=(1u<<23)|(1u<<19);
}
int main(int argc,char **argv) {
    set_option("PSX_INPUT_ROUTE_FILE","authored-fixture");
    set_option("PSX_CD_DMA_MODEL","octoshock-2.2.2");
#ifdef PSX_TEST_SOURCE_CD_IMPLEMENTED
    if(argc==2) {
        setup(512,0);
        if(!strcmp(argv[1],"request-mode"))channels[3].chcr|=0x200;
        else if(!strcmp(argv[1],"capture-active")) {
            start_async_cdrom_transfer();start_source_cdrom();dma_snapshot_write(NULL);return 1;
        }
        execute_ch3_cdrom();return 1;
    }
#else
    (void)argc;(void)argv;
#endif
    setup(512,27);execute_ch3_cdrom();
    if(writes!=512 || irqs!=1 || psx_cycle_count!=4608) {
        fprintf(stderr,"manual CD source wait absent: words=%u irqs=%u cycle=%llu expected=512/1/4608\n",writes,irqs,(unsigned long long)psx_cycle_count);
        return 1;
    }
#ifdef PSX_TEST_SOURCE_CD_IMPLEMENTED
    for(uint32_t phase=0;phase<128;phase++) {
        setup(512,phase);start_async_cdrom_transfer();start_source_cdrom();
        assert(writes==8 && cdrom_async.remaining_words==504 && irqs==0);
        assert(ram[0x10000/4]==0xCA000000 && ram[0x10020/4]==0xCCCCCCCC);
        uint64_t expected=((phase+4536+127)/128)*128;
        psx_cycle_count=expected-1;advance_source_cdrom();
        assert(cdrom_async.active && irqs==0 && writes<512);
        psx_cycle_count++;advance_source_cdrom();
        assert(writes==512 && irqs==1 && !cdrom_async.active);
        assert(channels[3].madr==0x10800 && ram[0x107FC/4]==0xCA0001FF);
        psx_cycle_count+=256;advance_source_cdrom();assert(writes==512 && irqs==1);
    }
    for(uint32_t count=1;count<=17;count++) {
        setup(count,0);execute_ch3_cdrom();assert(writes==count && irqs==1);
        assert((psx_cycle_count==0)==(count<=8));
    }
    setup(512,27);channels[3].chcr|=0x100;execute_ch3_cdrom();
    assert(psx_cycle_count==27 && writes==8 && cdrom_async.active && irqs==0);
    psx_cycle_count=4608;advance_source_cdrom();assert(writes==512 && irqs==1);
    setup(1,43);channels[3].chcr=0x11400100;available=0;execute_ch3_cdrom();
    assert(psx_cycle_count==43 && writes==1 && ram[0x10000/4]==0 && irqs==1 && !cdrom_async.active);
    setup(12,0);available=2;execute_ch3_cdrom();
    assert(writes==12 && ram[0x10000/4]==0xCA000000 && ram[0x10004/4]==0xCA000001 && ram[0x10008/4]==0 && irqs==1);
    setup(0,0);execute_ch3_cdrom();assert(writes==65536 && irqs==1);
    assert(dma_snapshot_read(NULL,0)==0);
    set_option("PSX_CD_DMA_MODEL","");setup(512,0);execute_ch3_cdrom();
    assert(writes==0 && psx_cycle_count==0 && cdrom_async.active);
#endif
    puts("PASS source CD manual service phases, partial data, wait, IRQ, short/zero count, restore guard and default control");return 0;
}

/* LL option is off in this adjacent fixture. */
int gpu_dma_source_ll_ready(void) {abort();}

/* Source GPU projection is inactive in this isolated controller fixture. */
int source_gpu_runtime_active(void) {return 0;}
void source_gpu_runtime_copy(SourceGPUServiceClock *clock, SourceGPUCommandProjection *command) {
    (void)clock; (void)command;
    assert(!"inactive source GPU must not be sampled");
}
void source_gpu_runtime_dma_write(void) {}
