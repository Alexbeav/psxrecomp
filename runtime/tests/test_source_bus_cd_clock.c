/* Real CD controller, shared DMA and production event scheduler. The only
 * disc backing is the maintained authored synthetic sector formula. Other
 * devices are explicitly inactive; no retail media or default timing oracle. */
#define PSX_TEST_REAL_CYCLE_SCHEDULER 1
#define PSX_TEST_REAL_DMA 1
#include "cdrom_source_fixture_support.c"
#include "source_fixture_assert.h"
#include "dma.h"
#include "psx_cycles.h"
#include "source_gpu_runtime.h"
extern uint32_t dma_snapshot_bytes(void);
extern void dma_snapshot_write(uint8_t *p);
extern int dma_snapshot_read(const uint8_t *p,uint32_t n);
int psx_get_in_exception(void){return 0;}
static uint32_t ram[0x80000];
uint32_t g_psx_ram_size=0x200000,g_psx_ram_mask=0x1fffff,i_mask;
int g_ls_replay_active; /* Freeze/render flags belong to real psx_cycles.c. */
int g_boot_state_cpu_timing_restored;
int g_psx_call_bail,g_precise_mode,g_ls_mode,g_source_gpu_runtime_enabled;
uint64_t g_guest_store_count,g_mmio_access_count;
void dirty_ram_ld_delay_discard(void){}
void dirty_ram_irq_ambient_resync_after_restore(void){}
extern void psx_cycles_resync_after_restore(CPUState *cpu);
uint64_t g_io_openbus_reads,g_io_openbus_writes;
uint8_t *memory_get_ram_ptr(void){return (uint8_t*)ram;}
uint32_t psx_read_word(uint32_t a){return ram[(a&0x1ffffc)/4];}
void psx_write_word(uint32_t a,uint32_t v){ram[(a&0x1ffffc)/4]=v;}
int source_gpu_runtime_active(void){return 0;}
void source_gpu_runtime_advance(void){}
uint32_t source_gpu_runtime_cycles_to_event(void){return UINT32_MAX;}
void source_gpu_runtime_dma_write(void){}
void gpu_queue_service(void){}
uint32_t gpu_queue_cycles_to_event(void){return UINT32_MAX;}
void sio_advance(uint32_t n){(void)n;}
uint32_t sio_cycles_to_irq(uint32_t mask){(void)mask;return UINT32_MAX;}
void timers_advance(uint32_t n){(void)n;}
uint32_t timers_cycles_to_irq(uint32_t mask){(void)mask;return UINT32_MAX;}
void interrupts_advance_cycles(uint32_t n){(void)n;}
uint32_t interrupts_cycles_to_vblank(void){return UINT32_MAX;}
void interrupts_service_scheduled_events(void){}
uint32_t psx_spu_sample_event_cycles_to_next(void){return UINT32_MAX;}
void psx_spu_sample_event_service(void){}
void starvation_watchdog_check(void){}
void starvation_ring_pc_sample(void){}
void psx_cycle_sample_check(uint64_t n){(void)n;}
int mdec_source_active(void){return 0;}
void overlay_capture_before_dma(uint32_t a,uint32_t n){(void)a;(void)n;}
void overlay_capture_on_dma(uint32_t a,uint32_t n,const uint8_t *p){(void)a;(void)n;(void)p;}
void dirty_ram_mark_executable_range(uint32_t a,uint32_t n){(void)a;(void)n;}
static void step(uint32_t n){psx_advance_cycles_slow(n);psx_devices_service_to_now();}
static int selected_fifo_position(void){return s_source_fifo_model?s_source_fifo_rd:RB_.pos;}
int main(void){
 set_model("PSX_INPUT_ROUTE_FILE","authored-fixture");
 set_model("PSX_CD_DMA_MODEL","octoshock-2.2.2");
 set_model("PSX_GPU_DMA_MODEL","");set_model("PSX_MDEC_SOURCE_MODEL","");
 set_model("PSX_INPUT_ROUTE_DMA_MODEL","");
 unsigned cases=0;
 for(int conservative=0;conservative<=1;conservative++)for(int bytes=0;bytes<=37;bytes++){
  psx_cycles_reset_for_boot();g_event_step_conservative=conservative;
  cdrom_init("synthetic");dma_init();memset(ram,0xcc,sizeof ram);i_stat=0;
  /* Seed exactly the controller's announced FIFO, then use public DMA/MMIO.
   * Byte tails deliberately exercise source padded reads vs default words. */
  request_reg=CDROM_REQUEST_BFRD;
  assert(s_source_fifo_model); /* PSX_CD_DMA_MODEL selected this actual owner. */
  s_source_fifo_rd=0;s_source_fifo_end=bytes;
  for(int j=0;j<bytes;j++)s_source_fifo[j]=(uint8_t)(0x10+j);
  dma_write(0x1f8010f0,8u<<12);dma_write(0x1f8010f4,(1u<<23)|(1u<<19));
  dma_write(0x1f8010b0,0x10000);dma_write(0x1f8010b4,12);
  /* Chopping avoids the CPU-blocking kick, so checkpoints retain ownership. */
  dma_write(0x1f8010b8,0x11000100);
  uint32_t dn=dma_snapshot_bytes(),cn=cdrom_snapshot_bytes();
  uint8_t *dw=malloc(dn),*cw=malloc(cn);assert(dw&&cw);
  for(int op=0;op<5;op++){
   dma_snapshot_write(dw);cdrom_snapshot_write(cw);
   uint64_t at=psx_cycle_count;uint32_t saved[12];memcpy(saved,ram+0x4000,sizeof saved);
   uint32_t irq=i_stat;step(128);uint32_t after[12];memcpy(after,ram+0x4000,sizeof after);
   uint32_t ch=dma_read(0x1f8010b8),irq_after=i_stat;int pos=selected_fifo_position();
   psx_cycle_count=at;assert(cdrom_snapshot_read(cw,cn));assert(dma_snapshot_read(dw,dn));
   memcpy(ram+0x4000,saved,sizeof saved);i_stat=irq;
   /* Scheduler explicitly reanchors after the rewind, just as native load does. */
   psx_cycles_resync_after_restore(NULL);step(128);
   assert(!memcmp(after,ram+0x4000,sizeof after));assert(ch==dma_read(0x1f8010b8));
   assert(irq_after==i_stat&&pos==selected_fifo_position());
  }
  assert(!(dma_read(0x1f8010b8)&0x01000000));assert(i_stat&(1u<<3));
  for(int word=0;word<12;word++){
   uint32_t expected=0;for(int b=0;b<4;b++)if(word*4+b<bytes)expected|=(uint32_t)(0x10+word*4+b)<<(8*b);
   if(ram[0x4000+word]!=expected)
    fprintf(stderr,"CD source first difference conservative=%d bytes=%d word=%d actual=%08x expected=%08x fifo=%d/%d CHCR=%08x cycle=%llu\n",conservative,bytes,word,ram[0x4000+word],expected,selected_fifo_position(),bytes,dma_read(0x1f8010b8),(unsigned long long)psx_cycle_count);
   assert(ram[0x4000+word]==expected);
  }
  assert(selected_fifo_position()==bytes);free(dw);free(cw);++cases;
 }
 printf("source real CD/DMA/event clock cases=%u checkpoints=%u exact data/CHCR/IRQ/FIFO PASS\n",cases,cases*5);return 0;
}
