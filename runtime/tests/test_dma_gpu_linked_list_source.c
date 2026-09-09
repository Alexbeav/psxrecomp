/* Authored production-controller fixture. No BIOS, disc or retail code. */
#define _POSIX_C_SOURCE 200809L
#include "../src/dma.c"
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
void gpu_set_gp0_linked_list_node(uint32_t a,uint32_t b) {(void)a;(void)b;}
uint32_t gpu_read_gpuread(void) {abort();}
void gpu_set_gp0_source(uint32_t a) {(void)a;}
static int ready_state=1;
int gpu_dma_source_ll_ready(void) {return ready_state;}
void gpu_write_gp0(uint32_t v) {uploaded[upload_count++]=v;}
uint32_t gpu_dma_vram_upload_words(void) {return upload_left;}
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) {(void)a;}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) {return a;}
void spu_dma_write(uint32_t v) {(void)v;abort();}
uint32_t spu_dma_read(void) {abort();}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;abort();}

static void setup(uint32_t words,int ready) {
    dma_init();memset(ram,0,sizeof(ram));irqs=i_stat=upload_count=0;
    ready_state=ready;psx_cycle_count=27;psx_next_service_cycle=0;
    ram[0x1000/4]=(words<<24)|0xffffff;
    channels[2].madr=0x1000;channels[2].bcr=0;
    channels[2].chcr=0x01000401;dpcr|=8u<<8;dicr=(1u<<23)|(1u<<18);
}
int main(int argc,char **argv) {
    set_option("PSX_INPUT_ROUTE_FILE","authored-fixture");
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
    set_option("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-linked-list");
#else
    set_option("PSX_GPU_DMA_MODEL","");
#endif
    if(argc==2) {
        setup(65,1);
        if(!strcmp(argv[1],"parser"))ready_state=-1;
        else if(!strcmp(argv[1],"draw"))ram[0x1004/4]=0x20000000;
        else if(!strcmp(argv[1],"reverse"))channels[2].chcr&=~1u;
        else if(!strcmp(argv[1],"address"))channels[2].madr=0x800000;
        else if(!strcmp(argv[1],"replace")){try_execute(2);dma_write(0x1f8010a0,0x2000);return 1;}
        else if(!strcmp(argv[1],"dpcr")){try_execute(2);dma_write(0x1f8010f0,dpcr^0x800);return 1;}
        else if(!strcmp(argv[1],"capture")){try_execute(2);dma_snapshot_write(NULL);return 1;}
        try_execute(2);return 1;
    }
    /* Ten independent original-DLL expectations: GPU-ready x 0/6/49/50/65.
     * Only immediate completion/busy and eventual release are asserted here.
     * Source receipt owns exact guest read timing and source GPU behavior. */
    const unsigned words[]={0,6,49,50,65};
    for(int ready=0;ready<2;ready++)for(unsigned i=0;i<5;i++) {
        unsigned n=words[i];setup(n,ready);try_execute(2);
        unsigned expected=ready&&n<=49;
        if(irqs!=expected || !!(channels[2].chcr&(1u<<24))==expected ||
           (!ready && upload_count)) {
            fprintf(stderr,"source startup mismatch ready=%d words=%u irq=%u expected=%u busy=%u sent=%u\n",ready,n,irqs,expected,!!(channels[2].chcr&(1u<<24)),upload_count);return 1;
        }
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
        psx_cycle_count=512;advance_source_gpu_ll();
        if(!ready){assert(!upload_count&&!irqs);ready_state=1;}
        psx_cycle_count=640;advance_source_gpu_ll();
        assert(upload_count==n&&irqs==1&&channels[2].chcr==0x401);
        psx_cycle_count=768;advance_source_gpu_ll();assert(irqs==1&&upload_count==n);
#endif
    }
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
    /* Generic PSX-DMA-001 ownership: a future payload and future node remain
     * live after kick. Neither may be snapshotted by a metadata prepass. */
    setup(65,1);try_execute(2);assert(upload_count==49);
    ram[(0x1004+49*4)/4]=0xe1000001;
    psx_cycle_count=128;advance_source_gpu_ll();assert(uploaded[49]==0xe1000001&&irqs==1);
    setup(49,1);ram[0x1000/4]=(49u<<24)|0x2000;ram[0x2000/4]=0xffffff;
    try_execute(2);assert(upload_count==49&&!irqs);
    ram[0x2000/4]=(1u<<24)|0xffffff;ram[0x2004/4]=0xe2000002;
    psx_cycle_count=128;advance_source_gpu_ll();assert(upload_count==50&&uploaded[49]==0xe2000002&&irqs==1);
    /* Source does not bank positive credit while not ready. */
    setup(255,0);try_execute(2);psx_cycle_count=512;advance_source_gpu_ll();
    ready_state=1;psx_cycle_count=640;advance_source_gpu_ll();assert(upload_count==113&&!irqs);
    /* Partial service precedes DICR replacement: completion sees old mask. */
    setup(50,1);dicr=1u<<23;try_execute(2);psx_cycle_count=28;
    dma_write(0x1f8010f4,(1u<<23)|(1u<<18));assert(upload_count==50&&!irqs);
    setup(50,1);try_execute(2);psx_cycle_count=28;
    dma_write(0x1f8010f4,0);assert(upload_count==50&&irqs==1);
    assert(!dma_snapshot_read(NULL,0));
    set_option("PSX_GPU_DMA_MODEL","");setup(6,1);try_execute(2);
    assert(upload_count==6&&!irqs&&delayed_complete[2].active);
#endif
    puts("PASS ten source startup states,49/50 boundary,live future words/nodes,stalls,write ordering,default");return 0;
}

/* Source GPU projection is inactive in this isolated controller fixture. */
int source_gpu_runtime_active(void) {return 0;}
void source_gpu_runtime_dma_write(void) {}
