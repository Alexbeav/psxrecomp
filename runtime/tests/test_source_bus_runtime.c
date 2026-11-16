/* Combined production GPU dispatch/raster, shared DMA, MDEC and SPU. The CD
 * endpoint is an authored ready sector stream; CD command timing is covered
 * by the maintained independent CD transcript suite. No BIOS/disc/program. */
#define _POSIX_C_SOURCE 200809L
#include "source_fixture_assert.h"
#include "../src/gpu.c"
#include "source_bus_runtime_seams.inc"
#include "gpu_hd_textures.h"
#include "source_fixture_assert.h"
#include "dma.h"
#include "mdec.h"
#include "cdrom.h"
#include "spu.h"
#include "psx_cycles.h"
#include "psx_icache.h"
#undef assert
#define assert(x) ((x)?(void)0:psx_source_fixture_fail(#x,__LINE__))
uint64_t psx_next_service_cycle;
uint64_t g_psx_cycle_fast_limit,g_psx_device_gen;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
uint32_t g_psx_gcs_recip_q16=65536u,g_psx_gcs_frac;
uint32_t *g_psx_cyc_local_acc;
void (*g_psx_cpu_step_boundary_callback)(CPUState *,uint32_t,uint64_t);
int g_ls_replay_active,g_event_step_conservative,psx_in_device_service;
int g_psx_guest_time_frozen;
uint32_t i_stat,i_mask;
static uint64_t serviced;
/* This finite device fixture has no recorded input endpoint. */
uint32_t input_route_observer_terminal_frame(void){return 0;}
static uint32_t cd_cursor,cd_reads;
void psx_irq_raise(uint32_t b,uint32_t detail){(void)detail;i_stat|=1u<<b;}
int psx_get_in_exception(void){return 0;}
void psx_write_word(uint32_t a,uint32_t v){assert((a&0x1ffffc)/4<sizeof test_ram/sizeof *test_ram);test_ram[(a&0x1ffffc)/4]=v;}
uint64_t psx_get_cycle_count(void){return psx_cycle_count+g_psx_cyc_batch;}
void psx_devices_service_to_now(void){
#ifdef PSX_TEST_FROZEN_SOURCE
 if(g_psx_guest_time_frozen || g_psx_render_pass_active){fprintf(stderr,"SOURCE_FROZEN_BOUNDARY forbidden device service frozen=%d render=%d cycle=%llu\n",g_psx_guest_time_frozen,g_psx_render_pass_active,(unsigned long long)psx_cycle_count);exit(5);}
#endif
 if(psx_in_device_service)return;psx_in_device_service=1;
 uint32_t elapsed=(uint32_t)(psx_cycle_count-serviced);
 source_gpu_runtime_advance();gpu_queue_service();dma_advance(elapsed);spu_catch_up();serviced=psx_cycle_count;
 psx_next_service_cycle=psx_cycle_count+1;psx_in_device_service=0;
}
void psx_advance_cycles_slow(uint32_t n){while(n--){++psx_cycle_count;psx_devices_service_to_now();}}
static void to(uint64_t t){assert(t>=psx_cycle_count);psx_advance_cycles_slow((uint32_t)(t-psx_cycle_count));}
void psx_icache_fetch_miss(CPUState *cpu,uint32_t pc){(void)cpu;(void)pc;}
/* No host instruction observer is installed. Device boundaries remain owned
 * by source_gpu_runtime_cpu_boundary, independently of this observer seam. */
void psx_cpu_step_boundary_fn(CPUState *cpu,uint32_t pc){
 (void)cpu;(void)pc;assert(!g_psx_cpu_step_boundary_callback);
}
int cdrom_dma_ready(void){return 1;}
uint32_t cdrom_dma_sector_word_count(void){return 512;}
uint32_t cdrom_dma_read(void){++cd_reads;return 0xcd000000u+cd_cursor++;}
uint32_t cdrom_dma_read_padded(void){return cdrom_dma_read();}
void cdrom_debug_snapshot(CDROMDebugState *s){memset(s,0,sizeof *s);}
int cdrom_get_setloc_lba(void){return 0;}
void interrupts_raster_gp1(uint32_t w){(void)w;}
int interrupts_raster_gpu_status(uint32_t *b){(void)b;return 0;}
uint32_t interrupts_source_snapshot_bytes(void){return 0;}
void interrupts_source_snapshot_write(uint8_t *p){(void)p;}
int interrupts_source_snapshot_read(const uint8_t *p,uint32_t n,int apply){(void)p;(void)apply;return !n;}
void overlay_capture_before_dma(uint32_t a,uint32_t n){(void)a;(void)n;}
void overlay_capture_on_dma(uint32_t a,uint32_t n,const uint8_t *p){(void)a;(void)n;(void)p;}
void dirty_ram_mark_executable_range(uint32_t a,uint32_t n){(void)a;(void)n;}
static void option(const char *k,const char *v){
#ifdef _WIN32
 _putenv_s(k,v);
#else
 setenv(k,v,1);
#endif
}
extern int dma_snapshot_validate(const uint8_t *,uint32_t);
extern uint32_t dma_snapshot_bytes(void);
extern void dma_snapshot_write(uint8_t *);
extern int dma_snapshot_read(const uint8_t *,uint32_t);
extern uint32_t mdec_snapshot_bytes(void);
extern void mdec_snapshot_write(uint8_t *);
extern int mdec_snapshot_read(const uint8_t *,uint32_t);
static uint64_t hash(const void *p,size_t n){const uint8_t *b=p;uint64_t h=1469598103934665603ull;while(n--)h=(h^*b++)*1099511628211ull;return h;}
typedef struct Snap{uint8_t *gpu,*dma,*mdec,*spu,*ram,*vram,*spuram;uint32_t gn,dn,mn,sn,stat,cursor,reads;uint64_t cycle;}Snap;
static Snap save(void){Snap s={0};s.gn=gpu_snapshot_bytes();s.dn=dma_snapshot_bytes();s.mn=mdec_snapshot_bytes();s.sn=spu_snapshot_bytes();
#define SAVE(field,n,write) s.field=malloc(n);assert(s.field);write(s.field)
 SAVE(gpu,s.gn,gpu_snapshot_write);SAVE(dma,s.dn,dma_snapshot_write);SAVE(mdec,s.mn,mdec_snapshot_write);SAVE(spu,s.sn,spu_snapshot_write);
#undef SAVE
 s.ram=malloc(sizeof test_ram);s.vram=malloc(sizeof vram);s.spuram=malloc(spu_get_ram_bytes());assert(s.ram&&s.vram&&s.spuram);
 memcpy(s.ram,test_ram,sizeof test_ram);memcpy(s.vram,vram,sizeof vram);memcpy(s.spuram,spu_get_ram(),spu_get_ram_bytes());s.stat=i_stat;s.cursor=cd_cursor;s.reads=cd_reads;s.cycle=psx_cycle_count;return s;}
static void load(const Snap *s){psx_cycle_count=serviced=s->cycle;g_psx_cyc_batch=0;psx_next_service_cycle=0;
 assert(gpu_snapshot_validate(s->gpu,s->gn)&&dma_snapshot_validate(s->dma,s->dn));
 memcpy(test_ram,s->ram,sizeof test_ram);memcpy(vram,s->vram,sizeof vram);memcpy(spu_get_ram_ptr(),s->spuram,spu_get_ram_bytes());
 assert(mdec_snapshot_read(s->mdec,s->mn)&&dma_snapshot_read(s->dma,s->dn)&&gpu_snapshot_read(s->gpu,s->gn)&&spu_snapshot_read(s->spu,s->sn));i_stat=s->stat;cd_cursor=s->cursor;cd_reads=s->reads;}
static void drop(Snap *s){free(s->gpu);free(s->dma);free(s->mdec);free(s->spu);free(s->ram);free(s->vram);free(s->spuram);}
static uint64_t observation(void){uint64_t h=gpu_read_gpustat();for(unsigned ch=0;ch<7;++ch)for(unsigned r=0;r<3;++r)h=h*31+dma_read(0x1f801080+16*ch+4*r);h=h*31+dma_read(0x1f8010f4);h=h*31+i_stat;h=h*31+mdec_read(0x1f801824);h^=hash(test_ram,sizeof test_ram);h^=hash(vram,sizeof vram);h^=hash(spu_get_ram(),spu_get_ram_bytes());h=h*31+spu_read(0x1f801dae);return h;}
static void kick(unsigned ch,uint32_t addr,uint32_t bcr,uint32_t chcr){dma_write(0x1f801080+16*ch,addr);dma_write(0x1f801084+16*ch,bcr);dma_write(0x1f801088+16*ch,chcr);}

#ifdef PSX_TEST_GENERATED_SOURCE
#include "psx_cyc.h"
uint8_t *g_psx_ram=(uint8_t *)test_ram;
int g_ls_mode,g_psx_precise_slice,g_psx_load_delay=1,g_psx_cyc_bb_defer;
volatile int g_ds_recording;
void psx_icache_fetch(CPUState *cpu,uint32_t pc){(void)cpu;(void)pc;}
void debug_server_log_call_entry(uint32_t pc){(void)pc;}
void debug_server_cyc_observe(uint32_t pc){(void)pc;}
void psx_check_interrupts_at(CPUState *cpu,uint32_t pc){(void)pc;assert(!(cpu->cop0[12]&1));}
uint32_t psx_cyc_load_word_slow(CPUState *cpu,uint32_t addr,uint32_t rt,uint32_t deps){
 assert((addr&0x1fffffffu)<0x200000u);psx_cyc_base(cpu);psx_cyc_deps(cpu,deps);if(cpu->ld_which_t==rt)cpu->ld_which_t=0;psx_cyc_lds(cpu);
 cpu->read_absorb[cpu->read_absorb_which]=0;cpu->read_absorb_which=0;uint32_t wait=7+dma_cpu_read_penalty();cpu->ld_absorb=wait-2;if(!(cpu->read_fudge&32))wait-=2;psx_advance_cycles(wait);cpu->ld_which_t=(uint8_t)rt;return cpu->read_word(addr);
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-label"
#include PSX_TEST_GENERATED_SOURCE
#pragma GCC diagnostic pop
#endif
static void operation(unsigned op){
 switch(op){
 case 0:gpu_write_gp0(0xe3000000);gpu_write_gp0(0xe407ffff);break;
 case 1:to(98);dma_write(0x1f8010f0,0x0fedcba9);dma_write(0x1f8010f4,0x00ff0000);break;
 case 2:to(146);kick(2,0x1000,0,0x11000401);break;
 case 3:to(160);spu_write(0x1f801da6,0x200);kick(4,0x3000,0x40004,0x11000201);break;
 case 4:to(170);kick(3,0x4000,32,0x11000000);break;
 case 5:to(180);kick(6,0x5000,256,0x11000002);break;
 case 6:{CPUState cpu={0};cpu.read_word=psx_read_word;cpu.read_fudge=cpu.ld_which_t=32;test_ram[0x6000/4]=0x48000000;uint64_t t=psx_cycle_count;source_gpu_runtime_cpu_boundary(&cpu,0x6000);assert(psx_cycle_count==t&&dma_cpu_source_halted());test_ram[0x6000/4]=0;
#ifdef PSX_TEST_GENERATED_SOURCE
 cpu.gpr[4]=0x80005000u-255u*4u;cpu.gpr[31]=0x80009000u;
 PSX_TEST_GENERATED_ENTRY(&cpu);psx_cyc_batch_flush();
 printf("SOURCE_BUS_GENERATED_GUEST at=%08x expected=00ffffff halt=%d cycle=%llu\n",cpu.gpr[1],dma_cpu_source_halted(),(unsigned long long)psx_cycle_count);
 assert(cpu.gpr[1]==0x00ffffffu && !dma_cpu_source_halted());
#else
 source_gpu_runtime_cpu_boundary(&cpu,0x6000);assert(!dma_cpu_source_halted()&&psx_cycle_count>t&&cpu.gpr[1]==0);
#endif
break;}
 case 7:to(4096);assert(cd_reads==32&&test_ram[0x4000/4]==0xcd000000&&test_ram[(0x4000+31*4)/4]==0xcd00001f);assert(test_ram[0x5000/4]==0x4ffc&&test_ram[(0x5000-255*4)/4]==0xffffff);break;
 case 8:to(8192);assert((dma_read(0x1f8010a8)&0x1000000)==0);assert(gpu_vram_peek(1,1)!=0);break;
 case 9:gpu_write_gp0(0xa0000000);gpu_write_gp0(0x00400040);gpu_write_gp0(0x00100010);break;
 case 10:to(8320);kick(2,0x7000,0x10080,0x11000201);break;
 case 11:to(8500);break;
 case 12:to(16384);assert(gpu_vram_peek(64,64)==0x1234&&gpu_vram_peek(65,64)==0x5678);break;
 case 13:mdec_write(0x1f801824,0x60000000);kick(0,0x8000,0x10011,0x11000201);break;
 case 14:to(16640);break;
 case 15:to(32768);break;
 default:abort();
 }
}
int main(void){option("PSX_INPUT_ROUTE_FILE","authored-fixture");option("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-quad");option("PSX_CD_DMA_MODEL","octoshock-2.2.2");option("PSX_INPUT_ROUTE_DMA_MODEL","octoshock-2.2.2-otc");option("PSX_MDEC_SOURCE_MODEL","octoshock-2.3");
 memset(test_ram,0,sizeof test_ram);mdec_init();dma_init();spu_init();gr_init(vram);gpu_init();source_gpu_runtime_init();
 const uint32_t node[]={0x05ffffff,0x280000ff,0,32,32u<<16,32u|(32u<<16)};memcpy(test_ram+0x1000/4,node,sizeof node);
 for(unsigned i=0;i<128;++i)test_ram[0x7000/4+i]=0x56781234;for(unsigned i=0;i<16;++i)test_ram[0x3000/4+i]=0x33221100+i;
 test_ram[0x8000/4]=0x40000001;for(unsigned i=1;i<17;++i)test_ram[0x8000/4+i]=0x01010101;
 Snap start=save();uint64_t expected[16];for(unsigned i=0;i<16;++i){operation(i);expected[i]=observation();}
 load(&start);for(unsigned i=0;i<16;++i){operation(i);assert(observation()==expected[i]);Snap s=save();to(psx_cycle_count+256);load(&s);assert(observation()==expected[i]);drop(&s);}drop(&start);
 Snap s=save();uint8_t *bad=malloc(s.gn);assert(bad);memcpy(bad,s.gpu,s.gn);bad[gpu_snapshot_base_bytes()]=0;assert(!gpu_snapshot_validate(bad,s.gn));assert(!gpu_snapshot_read(bad,s.gn));assert(observation()==expected[15]);free(bad);drop(&s);
#ifdef PSX_TEST_SOURCE_PREFIX
 {Snap base=save();uint8_t *bad=malloc(base.gn),*after=malloc(base.gn);assert(bad&&after);
  const unsigned offsets[]={0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,100,104,108,112,116,120,124,128,132,140,208,212,226,230,236,240,244,246,248,250,256,260,264,266,268,270,276,280,284,288};
  for(unsigned i=0;i<sizeof offsets/sizeof *offsets;++i){memcpy(bad,base.gpu,base.gn);memset(bad+offsets[i],0x7f,4);int admitted=gpu_snapshot_validate(bad,base.gn);if(admitted)fprintf(stderr,"invalid GPU prefix offset %u accepted\n",offsets[i]);assert(!admitted);assert(!gpu_snapshot_read(bad,base.gn));gpu_snapshot_write(after);assert(!memcmp(after,base.gpu,base.gn));}
  free(bad);free(after);drop(&base);printf("SOURCE_GPU_PREFIX malformed-fields=55 rejected-without-mutation PASS\n");
  Snap dm=save();bad=malloc(dm.dn);after=malloc(dm.dn);assert(bad&&after);const unsigned dma_offsets[]={92,98,146,147,166,190};
  for(unsigned i=0;i<sizeof dma_offsets/sizeof *dma_offsets;++i){memcpy(bad,dm.dma,dm.dn);bad[dma_offsets[i]]=255;assert(!dma_snapshot_validate(bad,dm.dn));assert(!dma_snapshot_read(bad,dm.dn));dma_snapshot_write(after);assert(!memcmp(after,dm.dma,dm.dn));}
  free(bad);free(after);drop(&dm);puts("SOURCE_DMA_PREFIX malformed-fields=6 rejected-without-mutation PASS");}

#endif
#ifdef PSX_TEST_FROZEN_SOURCE
 for(unsigned mode=0;mode<2;++mode){
  kick(6,0x9000,256,0x11000002);assert(dma_cpu_source_halted());
  CPUState cpu={0};cpu.read_word=psx_read_word;cpu.gpr[1]=0x12345678;CPUState old=cpu;
  Snap before=save();
  if(mode)g_psx_render_pass_active=1;else g_psx_guest_time_frozen=1;
  printf("SOURCE_FROZEN_BOUNDARY before mode=%u cycle=%llu halt=%d\n",mode,(unsigned long long)psx_cycle_count,dma_cpu_source_halted());
  source_gpu_runtime_cpu_boundary(&cpu,0x6000);
  assert(psx_cycle_count==before.cycle && !memcmp(&cpu,&old,sizeof cpu));
  uint8_t *device=malloc(before.dn);assert(device);dma_snapshot_write(device);assert(!memcmp(before.dma,device,before.dn));free(device);
  uint8_t *graphics=malloc(before.gn);assert(graphics);gpu_snapshot_write(graphics);assert(!memcmp(before.gpu,graphics,before.gn));free(graphics);
  /* The boundary guard must not suppress an allowed transactional GPU draw. */
  gpu_write_gp0(0x020000ff);gpu_write_gp0(100u|(100u<<16));gpu_write_gp0(16u|(1u<<16));assert(gpu_vram_peek(100,100)==31);
  assert(psx_cycle_count==before.cycle);
  g_psx_render_pass_active=g_psx_guest_time_frozen=0;load(&before);drop(&before);
  source_gpu_runtime_cpu_boundary(&cpu,0x6000);assert(!dma_cpu_source_halted()&&cpu.gpr[1]==old.gpr[1]);
 }
 puts("SOURCE_FROZEN_BOUNDARY modes=2 unchanged-clock/device/CPU + transactional-draw + ordinary-wait PASS");
#endif
 printf("source bus combined: 16 active operations/cold+per-operation rewind PASS; GPU/DMA/SPU/MDEC real, CD endpoint authored\n");return 0;}

void gpu_hd_textures_set_vram(const uint16_t *p){(void)p;}
void spu_shadow_reset(void){}
int spu_shadow_enabled(void){return 0;}
void spu_shadow_process(int16_t *samples,int frames){(void)samples;(void)frames;}

int gpu_hd_textures_active(void){return 0;}
int gpu_hd_textures_dump_enabled(void){return 0;}
