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
UNUSED_DMA_STUB(gpu_set_gp0_linked_list_node)
uint32_t gpu_read_gpuread(void) {abort();}
void gpu_set_gp0_source(uint32_t a) {(void)a;}
void gpu_write_gp0(uint32_t v) {assert(upload_left);upload_left--;uploaded[upload_count++]=v;}
uint32_t gpu_dma_vram_upload_words(void) {return upload_left;}
void gpu_ws_begin_linked_list(void) {abort();}
void gpu_ws_end_linked_list(void) {abort();}
void gpu_ws_prepass_linked_list(uint32_t a) {(void)a;abort();}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) {(void)a;abort();}
void spu_dma_write(uint32_t v) {(void)v;abort();}
uint32_t spu_dma_read(void) {abort();}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;abort();}
static void setup(uint32_t blocks,uint32_t words,uint64_t phase) {
    dma_init();memset(ram,0,sizeof(ram));irqs=i_stat=upload_count=0;
    for(uint32_t i=0;i<65536;i++)ram[0x10000/4+i]=0xAB000000+i;
    upload_left=blocks*words;psx_cycle_count=phase;psx_next_service_cycle=0;
    channels[2].madr=0x10000;channels[2].bcr=(blocks<<16)|words;
    channels[2].chcr=0x01000201;dpcr|=8u<<8;dicr=(1u<<23)|(1u<<18);
}
int main(int argc,char **argv) {
    set_option("PSX_INPUT_ROUTE_FILE","authored-fixture");
    set_option("PSX_GPU_DMA_MODEL","octoshock-2.2.2-vram-upload");
    setup(12,16,27);
#ifdef PSX_TEST_SOURCE_GPU_IMPLEMENTED
    if(argc==3 && !strcmp(argv[1],"--oracle")) {
        FILE *f=fopen(argv[2],"r");assert(f);
        for(unsigned line=0;line<512;line++) {
            unsigned phase,t,count,madr,bcr,chcr,flags;
            assert(fscanf(f,"%u %u %u %u %u %u %u",&phase,&t,&count,&madr,&bcr,&chcr,&flags)==7);
            if(line%4==0){setup(12,16,phase);try_execute(2);}
            psx_cycle_count=t;advance_source_gpu();
            if(upload_count!=count || channels[2].madr!=madr || channels[2].bcr!=bcr ||
               channels[2].chcr!=chcr || irqs!=!!(flags&4)) {
                fprintf(stderr,"source oracle mismatch phase=%u time=%u words=%u expected=%u IRQ=%u expected=%u\n",phase,t,upload_count,count,irqs,!!(flags&4));return 1;
            }
        }
        for(unsigned i=0;i<2;i++) {
            char kind[8];unsigned t,count,chcr,flags;
            assert(fscanf(f,"%7s %u %u %u %u",kind,&t,&count,&chcr,&flags)==5&&!strcmp(kind,"WRITE"));
            setup(12,16,127);dicr=1u<<23;try_execute(2);
            psx_cycle_count=128;advance_source_gpu();psx_cycle_count=256;advance_source_gpu();
            psx_cycle_count=t;dma_write(0x1f8010f4,(1u<<23)|(1u<<18));
            assert(upload_count==count && channels[2].chcr==chcr && irqs==!!(flags&4));
            assert(fscanf(f,"%7s %u %u %u %u",kind,&t,&count,&chcr,&flags)==5&&!strcmp(kind,"END"));
            psx_cycle_count=384;advance_source_gpu();
            assert(upload_count==count && channels[2].chcr==chcr && irqs==!!(flags&4));
        }
        unsigned extra;assert(fscanf(f,"%u",&extra)==EOF);fclose(f);
        puts("PASS exact-source512phase states and two completion/write-order pairs");return 0;
    }
    if(argc==2) {
        if(!strcmp(argv[1],"no-upload"))upload_left=0;
        else if(!strcmp(argv[1],"short-upload"))upload_left=191;
        else if(!strcmp(argv[1],"reverse"))channels[2].chcr&=~1u;
        else if(!strcmp(argv[1],"chopped"))channels[2].chcr|=0x100;
        else if(!strcmp(argv[1],"replace")) {try_execute(2);dma_write(0x1f8010a0,0x20000);return 1;}
        else if(!strcmp(argv[1],"dpcr")) {try_execute(2);dma_write(0x1f8010f0,dpcr^(8u<<8));return 1;}
        else if(!strcmp(argv[1],"capture")) {try_execute(2);dma_snapshot_write(NULL);return 1;}
        try_execute(2);return 1;
    }
#else
    (void)argc;(void)argv;
#endif
    try_execute(2);
    if(upload_count!=43 || psx_cycle_count!=27 || irqs) {
        fprintf(stderr,"GPU upload source start absent: words=%u cycle=%llu irq=%u expected43/27/0\n",upload_count,(unsigned long long)psx_cycle_count,irqs);return 1;
    }
#ifdef PSX_TEST_SOURCE_GPU_IMPLEMENTED
    assert(dma_cpu_read_penalty()==15);
    assert(channels[2].bcr==0x00090010 && channels[2].madr==0x10080);
    assert(uploaded[42]==0xAB00002A);
    ram[0x10000/4+43]=0xDEADBEEF; /* unread future RAM must remain live */
    psx_cycle_count=127;advance_source_gpu();assert(upload_count==43);
    psx_cycle_count=128;advance_source_gpu();assert(upload_count>43 && uploaded[43]==0xDEADBEEF && !irqs);
    psx_cycle_count=255;advance_source_gpu();assert(upload_count<192 && dma_cpu_read_penalty()==15);
    psx_cycle_count=256;advance_source_gpu();assert(upload_count==192 && irqs==1 && dma_cpu_read_penalty()==0);
    assert(channels[2].madr==0x10300 && channels[2].bcr==16 && !(channels[2].chcr&(1u<<24)));
    psx_cycle_count=512;advance_source_gpu();assert(upload_count==192 && irqs==1);
    for(uint32_t phase=0;phase<128;phase++) {
        setup(12,16,phase);try_execute(2);
        assert(upload_count==43 && dma_cycles_to_internal_event()==128-phase);
        /* Independent source oracle establishes 276 total clocks for this
         * transfer, of which64 are supplied at kick. Service uses actual
         * elapsed time; the first partial interval is not a full128. */
        uint32_t end=((phase+212u+127u)/128u)*128u;
        psx_cycle_count=end-1;advance_source_gpu();assert(upload_count<192);
        psx_cycle_count=end;advance_source_gpu();assert(upload_count==192 && irqs==1);
    }
    setup(1,1,0);try_execute(2);assert(upload_count==1 && irqs==1 && dma_cpu_read_penalty()==0);
    setup(2,256,0);try_execute(2);assert(dma_cpu_read_penalty()==200);
    assert(!dma_snapshot_read(NULL,0));
    set_option("PSX_GPU_DMA_MODEL","");setup(12,16,0);try_execute(2);
    assert(upload_count==192 && !irqs && dma_cpu_read_penalty()==0 && delayed_complete[2].active);
#endif
    puts("PASS source VRAM request DMA phases, live RAM, block overhead, read penalty, partial completion, IRQ and default");return 0;
}

/* LL option is off in this adjacent fixture. */
int gpu_dma_source_ll_ready(void) {abort();}

/* Source GPU projection is inactive in this isolated controller fixture. */
int source_gpu_runtime_active(void) {return 0;}
void source_gpu_runtime_dma_write(void) {}
