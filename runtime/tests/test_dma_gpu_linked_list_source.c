/* Authored production-controller fixture. No BIOS, disc or retail code. */
#define _POSIX_C_SOURCE 200809L
#include "../src/dma.c"
#include <assert.h>
/* Payload words the kick credit covers after one node header. */
#define LLW (DSM_KICK_CREDIT - DSM_LL_NODE_PAYLOAD)
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
    dsm_service(DSM_GPU, psx_cycle_count);
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
    dsm_service(DSM_LL, psx_cycle_count);
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
void gpu_ws_restore_linked_list_rank(uint32_t r) {(void)r;}
void gpu_ws_validate_linked_list_header(uint32_t a,uint32_t h) {(void)a;(void)h;}
void gpu_ws_validate_linked_list_node(uint32_t a,uint32_t n) {(void)a;(void)n;}
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
    /* Ten independent original-DLL expectations: GPU-ready x 0/6/W/W+1/65, with
     * W = the payload words the kick credit covers after one header (DSM_LL_NODE).
     * Only immediate completion/busy and eventual release are asserted here.
     * Source receipt owns exact guest read timing and source GPU behavior. */
    const unsigned words[]={0,6,LLW,LLW+1,65};
    for(int ready=0;ready<2;ready++)for(unsigned i=0;i<5;i++) {
        unsigned n=words[i];setup(n,ready);try_execute(2);
        unsigned expected=ready&&n<=LLW;
        if(irqs!=expected || !!(channels[2].chcr&(1u<<24))==expected ||
           (!ready && upload_count)) {
            fprintf(stderr,"source startup mismatch ready=%d words=%u irq=%u expected=%u busy=%u sent=%u\n",ready,n,irqs,expected,!!(channels[2].chcr&(1u<<24)),upload_count);return 1;
        }
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
        /* Capture while stalled or part-way through a linked list, then
         * continue the same live RAM payload after restoring both sections. */
        uint8_t base[512],source[512];
        assert(dma_snapshot_bytes()<=sizeof base && dma_src_wire_bytes()<=sizeof source);
        dma_snapshot_write(base);dma_src_wire_write(source);
        memset(&dsm[DSM_LL],0,sizeof dsm[DSM_LL]);
        memset(&channels[2],0,sizeof channels[2]);
        assert(dma_snapshot_read(base,dma_snapshot_bytes()));
        assert(dma_src_wire_read(source,dma_src_wire_bytes()));
        psx_cycle_count=512;dsm_service(DSM_LL, psx_cycle_count);
        if(!ready){assert(!upload_count&&!irqs);ready_state=1;}
        psx_cycle_count=640;dsm_service(DSM_LL, psx_cycle_count);
        assert(upload_count==n&&irqs==1&&channels[2].chcr==0x401);
        psx_cycle_count=768;dsm_service(DSM_LL, psx_cycle_count);assert(irqs==1&&upload_count==n);
#endif
    }
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
    /* Generic PSX-DMA-001 ownership: a future payload and future node remain
     * live after kick. Neither may be snapshotted by a metadata prepass. */
    setup(65,1);try_execute(2);assert(upload_count==LLW);
    ram[(0x1004+LLW*4)/4]=0xe1000001;
    psx_cycle_count=128;dsm_service(DSM_LL, psx_cycle_count);assert(uploaded[LLW]==0xe1000001&&irqs==1);
    setup(LLW,1);ram[0x1000/4]=((uint32_t)LLW<<24)|0x2000;ram[0x2000/4]=0xffffff;
    try_execute(2);assert(upload_count==LLW&&!irqs);
    ram[0x2000/4]=(1u<<24)|0xffffff;ram[0x2004/4]=0xe2000002;
    psx_cycle_count=128;dsm_service(DSM_LL, psx_cycle_count);assert(upload_count==LLW+1&&uploaded[LLW]==0xe2000002&&irqs==1);
    /* Source does not bank positive credit while not ready. */
    setup(255,0);try_execute(2);psx_cycle_count=512;dsm_service(DSM_LL, psx_cycle_count);
    ready_state=1;psx_cycle_count=640;dsm_service(DSM_LL, psx_cycle_count);assert(upload_count==128-DSM_LL_NODE_PAYLOAD&&!irqs);
    /* Partial service precedes DICR replacement: completion sees old mask.
     * The walk moves on 128-cycle edges (D23), so the write is at the edge. */
    setup(LLW+1,1);dicr=1u<<23;try_execute(2);psx_cycle_count=128;
    dma_write(0x1f8010f4,(1u<<23)|(1u<<18));assert(upload_count==LLW+1&&!irqs);
    setup(LLW+1,1);try_execute(2);psx_cycle_count=128;
    dma_write(0x1f8010f4,0);assert(upload_count==LLW+1&&irqs==1);
    assert(!dma_snapshot_read(NULL,0));
    /* [ORACLE FIXTURE D23] completion slices. Kick at cycle 32 (edges on the
     * 128 grid), so the oracle's slice points dt = 96 + 128j fall on edges.
     * a: 1 + N header-only nodes -> done at dt 224 / 608 / 2528 / 10208.
     * b: 16 nodes of k NOP words -> k 1,2: 224; 4,8: 352; 15: 480. */
    {
        static const struct { unsigned nodes, k, done; } d23[] = {
            {17,0,224},{65,0,608},{257,0,2528},{1025,0,10208},
            {16,1,224},{16,2,224},{16,4,352},{16,8,352},{16,15,480},
        };
        for (unsigned c = 0; c < sizeof d23 / sizeof d23[0]; c++) {
            dma_init();memset(ram,0,sizeof(ram));irqs=i_stat=upload_count=0;ready_state=1;
            uint32_t a = 0x10000;
            for (unsigned i = 0; i < d23[c].nodes; i++) {
                uint32_t next = i + 1 < d23[c].nodes ? a + 4 * (d23[c].k + 1) : 0xFFFFFF;
                ram[a / 4] = (d23[c].k << 24) | next;
                for (unsigned w = 0; w < d23[c].k; w++) ram[a / 4 + 1 + w] = 0;
                a += 4 * (d23[c].k + 1);
            }
            psx_cycle_count = 32; psx_next_service_cycle = 0;
            channels[2].madr = 0x10000; channels[2].bcr = 0; channels[2].chcr = 0x01000401;
            dpcr |= 8u << 8; dicr = (1u << 23) | (1u << 18);
            try_execute(2);
            unsigned done = 0;
            for (uint64_t t = 128; t < 32 + 20000 && !done; t += 128) {
                psx_cycle_count = t; dsm_service(DSM_LL, t);
                if (!(channels[2].chcr & (1u << 24))) done = (unsigned)(t - 32);
            }
            if (done != d23[c].done) {
                fprintf(stderr, "D23 nodes %u k %u: done at dt %u, oracle slice %u\n", d23[c].nodes, d23[c].k, done, d23[c].done);
                return 1;
            }
        }
    }
    /* The default model is the event-driven DMA2 walker (dma_gpu_ll.c): the kick
     * sends nothing synchronously, so the walk itself is live checkpoint state.
     * Restoring it into a cleared controller must resume the same walk. */
    set_option("PSX_GPU_DMA_MODEL","");setup(6,1);try_execute(2);
    assert(!upload_count&&!irqs&&gpu_linked_list.active);
    {
        uint8_t base[512];
        assert(dma_snapshot_bytes()<=sizeof base);
        dma_snapshot_write(base);
        DMAGPULinkedList live=gpu_linked_list;
        memset(&gpu_linked_list,0,sizeof gpu_linked_list);
        assert(dma_snapshot_read(base,dma_snapshot_bytes()));
        assert(!memcmp(&live,&gpu_linked_list,sizeof live));
    }
#endif
    puts("PASS ten source startup states,W/W+1 boundary,D23 completion slices,live future words/nodes,stalls,write ordering,default");return 0;
}

/* Source GPU projection is inactive in this isolated controller fixture. */
int source_gpu_runtime_active(void) {return 0;}
uint32_t source_gpu_runtime_cycles_to_event(void) {return UINT32_MAX;}
void source_gpu_runtime_copy(SourceGPUServiceClock *clock,SourceGPUCommandProjection *command) {
    (void)clock;(void)command;abort();
}
void source_gpu_runtime_dma_write(void) {}
