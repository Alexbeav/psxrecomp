#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cpu_state.h"
#include "psx_cycles.h"
#include "timers.h"
#include "../src/timers.c"
#include "../src/psx_cycles.c"
uint32_t i_stat,i_mask;
static unsigned irq_count;static uint64_t last_irq;
void psx_irq_raise(uint32_t bit,uint32_t detail){(void)detail;assert(bit==6);irq_count++;last_irq=psx_cycle_count;i_stat|=1u<<bit;}
void event_ring_record_aux(uint16_t type,uint8_t source,uint32_t aux){(void)type;(void)source;(void)aux;}
uint32_t interrupts_cycles_to_vblank(void){return 1000000;}
uint32_t cdrom_cycles_to_irq(uint32_t mask){(void)mask;return 1000000;}
uint32_t sio_cycles_to_irq(uint32_t mask){(void)mask;return 1000000;}
uint32_t dma_cycles_to_internal_event(void){return 1000000;}
uint32_t dma_cycles_to_deliverable_irq(uint32_t mask){(void)mask;return 1000000;}
uint32_t psx_spu_sample_event_cycles_to_next(void){return 768;}
void psx_spu_sample_event_service(void){}
void interrupts_service_scheduled_events(void){}
void sio_advance(uint32_t cycles){(void)cycles;}
void cdrom_advance(uint32_t cycles){(void)cycles;}
void dma_advance(uint32_t cycles){(void)cycles;}
void interrupts_advance_cycles(uint32_t cycles){(void)cycles;}
void starvation_watchdog_check(void){}
void starvation_ring_pc_sample(void){}
int psx_netplay_active(void){return 0;}
int psx_selfcheck_enabled(void){return 0;}
int psx_get_in_exception(void){return 0;}
void dirty_ram_ld_delay_discard(void){}
void dirty_ram_irq_ambient_resync_after_restore(void){}
uint64_t g_guest_store_count,g_mmio_access_count;
int g_ls_mode,g_precise_mode,g_psx_call_bail,g_ls_replay_active;
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"restore")){timers_init();uint16_t a[3]={0};uint32_t b[3]={0};int32_t c[3]={0};timers_set_snapshot(a,b,a,c,b);return 9;}
 if(argc>1 && !strcmp(argv[1],"timer0-irq")){timers_init();timers_write(0x1f801104,0x158);return 9;}
 unsigned op,v,time,c,m,t,d,irqs,irqtime,istat,next,rows=0,errors=0;
 while(scanf("%u %u %u %u %u %u %u %u %u %u %u",&op,&v,&time,&c,&m,&t,&d,&irqs,&irqtime,&istat,&next)==11){
  if(op==0){timers_init();psx_cycle_count=0;s_devices_synced_cycle=0;psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;g_psx_cyc_batch=0;psx_in_device_service=0;g_event_step_conservative=0;irq_count=last_irq=i_stat=i_mask=0;s_next_watchdog=s_next_pc_sample=UINT64_MAX;}
  else if(op==4)psx_advance_cycles(v);
  else if(op==6)i_stat=0;
  else {psx_devices_mmio_sync();timers_write(0x1f801120+(op==1?4:op==3?8:0),v);}
  psx_devices_mmio_sync();(void)timers_read(0x1f801120);
  uint16_t counters[3],targets[3];uint32_t modes[3],fractions[3];int32_t lines[3];timers_get_snapshot(counters,modes,targets,lines,fractions);
  if(psx_cycle_count!=time||counters[2]!=c||modes[2]!=m||targets[2]!=t||fractions[2]!=d||irq_count!=irqs||last_irq!=irqtime||i_stat!=istat||timers_cycles_to_irq(~0u)!=next){
   if(errors<4)fprintf(stderr,"row%u op%u/%u clock%llu/%u count%u/%u divider%u/%u IRQ%u/%u at%llu/%u next%u/%u\n",rows,op,v,(unsigned long long)psx_cycle_count,time,counters[2],c,fractions[2],d,irq_count,irqs,(unsigned long long)last_irq,irqtime,timers_cycles_to_irq(~0u),next);errors++;
  }rows++;
 }
 printf("%u exact-source timeline states; %u errors\n",rows,errors);return errors?1:0;
}

/* No source GPU consumer in this device-isolation control. */
void source_gpu_runtime_advance(void) {}
uint32_t source_gpu_runtime_cycles_to_event(void) {return UINT32_MAX;}
