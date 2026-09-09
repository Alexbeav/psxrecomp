/* Exact production sample-event functions plus real cycle service and SPU.
 * Adjacent devices are authored clock stubs; this does not model host audio.
 * Each case explicitly settles the starting latch before automatic service. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_cycles.h"
#include "../src/spu.c"
#include "../src/psx_cycles.c"
#ifndef PSX_TEST_SPU_SCHEDULER_INCLUDE
#error "Use verify_spu_scheduler.py to bind exact production source"
#endif
#include PSX_TEST_SPU_SCHEDULER_INCLUDE
uint64_t s_frame_count;
uint32_t i_stat,i_mask;
int g_ls_mode,g_precise_mode,g_psx_call_bail,g_ls_replay_active;
uint64_t g_guest_store_count,g_mmio_access_count;
static uint64_t timer_deadline,pump_time;
static unsigned pumps,timer_events,order;
void audio_trace_pcm(int t,const int16_t *s,int f){(void)t;(void)s;(void)f;}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b){(void)k;(void)a;(void)b;}
void psx_irq_raise(uint32_t b,uint32_t d){(void)d;i_stat|=1u<<b;}
uint32_t crc32_update(uint32_t c,const uint8_t *d,size_t n){(void)d;(void)n;return c;}
bool spu_shadow_enabled(void){return false;}
void spu_shadow_reset(void){}
void spu_shadow_process(int16_t *s,int n){(void)s;(void)n;}
uint32_t interrupts_cycles_to_vblank(void){return 1000000;}
uint32_t timers_cycles_to_irq(uint32_t m){(void)m;return timer_deadline>psx_cycle_count?(uint32_t)(timer_deadline-psx_cycle_count):UINT32_MAX;}
void timers_advance(uint32_t c){(void)c;if(timer_deadline && psx_cycle_count>=timer_deadline){timer_events++;order=order*10+1;timer_deadline=0;}}
uint32_t cdrom_cycles_to_irq(uint32_t m){(void)m;return 1000000;}
uint32_t sio_cycles_to_irq(uint32_t m){(void)m;return 1000000;}
uint32_t dma_cycles_to_internal_event(void){return 1000000;}
uint32_t dma_cycles_to_deliverable_irq(uint32_t m){(void)m;return 1000000;}
void interrupts_service_scheduled_events(void){}
void sio_advance(uint32_t c){(void)c;}
void cdrom_advance(uint32_t c){(void)c;}
void dma_advance(uint32_t c){(void)c;}
void interrupts_advance_cycles(uint32_t c){(void)c;}
void starvation_watchdog_check(void){}
void starvation_ring_pc_sample(void){}
int psx_netplay_active(void){return 0;}
int psx_selfcheck_enabled(void){return 0;}
int psx_get_in_exception(void){return 0;}
void dirty_ram_ld_delay_discard(void){}
void dirty_ram_irq_ambient_resync_after_restore(void){}
static void pump(void){int16_t stereo[2];pumps++;pump_time=psx_cycle_count;order=order*10+2;spu_render(stereo,1);}
static unsigned checks,failures;
static void expect(int value,const char *why,unsigned irq,unsigned phase){checks++;if(!value){if(failures<10)fprintf(stderr,"IRQ%u phase%u %s\n",irq,phase,why);failures++;}}
int main(void){
 for(unsigned irq=0;irq<2;irq++)for(unsigned phase=0;phase<768;phase++)for(unsigned adjacent=0;adjacent<2;adjacent++){
  spu_init();spu_write(0x1f801daa,0xc010u|(irq?0x40u:0));
  {int16_t stereo[2];spu_render(stereo,1);}
  psx_cycle_count=768+phase;s_devices_synced_cycle=psx_cycle_count;psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;g_psx_cyc_batch=0;psx_in_device_service=0;g_event_step_conservative=0;s_next_watchdog=s_next_pc_sample=UINT64_MAX;
  pumps=timer_events=order=0;pump_time=0;i_stat=i_mask=0;timer_deadline=0;
  psx_set_midframe_audio_pump(pump);psx_devices_mmio_sync();
  uint64_t cached=psx_next_service_cycle;
  expect(cached==1536,"preexisting sample deadline despite IRQ off",irq,phase);
  /* The timer is already due at the same boundary before this write. */
  if(adjacent){timer_deadline=1536;psx_devices_mmio_sync();}
  spu_write(0x1f801daa,0xc000u|(irq?0x40u:0));
  expect((spu_read(0x1f801dae)&63)==16,"write does not apply immediately",irq,phase);
  if(phase<767)psx_advance_cycles(767-phase);
  expect((spu_read(0x1f801dae)&63)==16 && pumps==0,"cached fast interval stays pending",irq,phase);
  psx_advance_cycles(1);
  expect((spu_read(0x1f801dae)&63)==0,"applied at automatic sample",irq,phase);
  expect(pumps==1 && pump_time==1536,"exactly one boundary pump",irq,phase);
  expect(timer_events==adjacent && order==(adjacent?12u:2u),"timer before SPU at common boundary",irq,phase);
 }
 /* A write exactly on a boundary follows device service. It must remain
  * pending until the next sample, including repeated writes at that cycle. */
 for(unsigned irq=0;irq<2;irq++){
  spu_init();spu_write(0x1f801daa,0xc010u|(irq?0x40u:0));
  {int16_t stereo[2];spu_render(stereo,1);}
  psx_cycle_count=s_devices_synced_cycle=767;psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
  timer_deadline=768;pumps=timer_events=order=0;psx_devices_mmio_sync();psx_advance_cycles(1);psx_devices_mmio_sync();
  expect(pumps==1 && order==12,"boundary services timer/sample before write",irq,0);
  spu_write(0x1f801daa,0xc001u|(irq?0x40u:0));spu_write(0x1f801daa,0xc000u|(irq?0x40u:0));
  expect((spu_read(0x1f801dae)&63)==16,"post-boundary writes remain pending",irq,0);
  psx_advance_cycles(767);expect((spu_read(0x1f801dae)&63)==16,"full interval after boundary write",irq,0);
  psx_advance_cycles(1);expect((spu_read(0x1f801dae)&63)==0 && pumps==2 && pump_time==1536,"next sample applies latest boundary write",irq,0);
 }
 printf("%u production-scheduler checks, %u failures\n",checks,failures);return failures?1:0;
}

/* No source GPU consumer in this device-isolation control. */
void source_gpu_runtime_advance(void) {}
uint32_t source_gpu_runtime_cycles_to_event(void) {return UINT32_MAX;}
