/* Exact production load helpers against an independent original-core oracle.
 * Fetch/base/dependency setup is outside this fixture; the supplied start is
 * the ReadMemory entry. No BIOS or retail instructions/data are used. */
#ifndef PSX_TEST_REAL_STEP
#define PSX_CYC_H /* Component mode excludes instruction step. */
#endif
#include "cpu_state.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint64_t psx_cycle_count;
static uint64_t sample_cycle;
static uint64_t g_psx_cycle_fast_limit;
#ifdef PSX_TEST_DEADLINE
int g_event_step_conservative=0;
#else
int g_event_step_conservative=1;
#endif
int g_ls_replay_active;
static unsigned phase,reads;
static int effect_test,device_flag,effect_order;
static uint64_t effect_deadline;
static void advance_clock(uint32_t n){
 psx_cycle_count+=n;
 if(effect_test && effect_deadline && psx_cycle_count>=effect_deadline){
  effect_order=effect_order*10+2;device_flag=1;effect_deadline=0;
 }
}
#ifdef PSX_TEST_REAL_STEP
uint64_t psx_next_service_cycle;
int psx_in_device_service,g_psx_cyc_bb_defer;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit,*g_psx_cyc_local_acc;
int g_psx_load_delay=1,g_ls_mode,g_ram_read_watch_active;
volatile int g_ds_recording;
uint32_t g_dma_cpu_read_wait;
uint8_t *g_psx_ram;
void psx_advance_cycles_slow(uint32_t n){advance_clock(n);}
void psx_devices_service_to_now(void){advance_clock(0);}
void debug_server_trace_ram_read_watch(uint32_t a,uint32_t b){(void)a;(void)b;}
int psx_load_delay_enabled(void){return 1;}
#else
static void psx_advance_cycles(uint32_t n){advance_clock(n);}
static void psx_cyc_base(CPUState*c){(void)c;}
static void psx_cyc_deps(CPUState*c,uint32_t mask){(void)c;(void)mask;}
static void psx_cyc_lds(CPUState*c){(void)c;}
static int psx_load_delay_enabled(void){return 1;}
#endif
static uint32_t dma_cpu_read_penalty(void){return 0;}
static uint32_t bus_read(uint32_t addr){
 assert((addr&0x1fffffff)==0x1f801120);sample_cycle=psx_cycle_count;reads++;
 if(effect_test){effect_order=effect_order*10+1;device_flag=0;}
 return (uint32_t)(sample_cycle+phase)/8;
}
uint32_t psx_read_word(uint32_t a){return bus_read(a);}
uint16_t psx_read_half(uint32_t a){return (uint16_t)bus_read(a);}
uint8_t psx_read_byte(uint32_t a){return (uint8_t)bus_read(a);}
#include PSX_TEST_READ_ORDER_INCLUDE
#ifdef PSX_TEST_GENERATED_READ_INCLUDE
#include PSX_TEST_GENERATED_READ_INCLUDE
#endif
static uint32_t perform_load(CPUState *c,unsigned width,unsigned lwc,uint32_t addr){
#ifdef PSX_TEST_GENERATED_READ_INCLUDE
 return generated_read(c,width,lwc,addr);
#elif defined(PSX_TEST_REAL_STEP)
 return lwc?psx_cyc_lwc2_read(c,addr):width==1?psx_cyc_load_byte(c,addr,3,0):width==2?psx_cyc_load_half(c,addr,3,0):psx_cyc_load_word(c,addr,3,0);
#else
 return lwc?psx_cyc_lwc2_read(c,addr):width==1?psx_cyc_load_byte(c,addr,3,0):width==2?psx_cyc_load_half_slow(c,addr,3,0):psx_cyc_load_word_slow(c,addr,3,0);
#endif
}
int main(void){
 unsigned width,lwc,fudge,p,start,value,sample,end,absorb,count=0,failures=0,value_failures=0;
 while(scanf("%u %u %u %u %u %u %u %u %u",&width,&lwc,&fudge,&p,&start,&value,&sample,&end,&absorb)==9){
  for(unsigned alias=0;alias<3;alias++){
   CPUState c;memset(&c,0,sizeof(c));c.read_fudge=fudge;c.ld_which_t=0x20;
   phase=p;psx_cycle_count=start;sample_cycle=0;reads=0;
#ifdef PSX_TEST_REAL_STEP
   c.ld_which_t=(uint8_t)fudge;
   if(!lwc)psx_cycle_count--; /* The source oracle begins after the base step. */
#ifdef PSX_TEST_DEADLINE
   psx_cycle_count-=5;g_psx_cyc_batch=5;psx_next_service_cycle=start+1;
#endif
#endif
   uint32_t addr=0x1f801120u|(alias==1?0x80000000u:alias==2?0xa0000000u:0);
   uint32_t v=perform_load(&c,width,lwc,addr);
   if(v!=value||sample_cycle!=sample||psx_cycle_count!=end||c.ld_absorb!=absorb||reads!=1){
    if(!failures || (v!=value && !value_failures))fprintf(stderr,"case%u alias%u width%u lwc%u fudge%u phase%u start%u: value%u/%u sample%llu/%u end%llu/%u absorb%u/%u reads%u\n",count,alias,width,lwc,fudge,p,start,v,value,(unsigned long long)sample_cycle,sample,(unsigned long long)psx_cycle_count,end,c.ld_absorb,absorb,reads);
    failures++;value_failures+=(v!=value);
   }
  }
  count++;
 }
 assert(count==512);printf("512 source vectors x3 aliases: failures=%u value_failures=%u\n",failures,value_failures);
 for(unsigned kind=0;kind<4;kind++){
  CPUState c;memset(&c,0,sizeof(c));c.ld_which_t=0;
  unsigned lwc=kind==3,width=kind==0?1:kind==1?2:4;
  phase=0;reads=0;psx_cycle_count=1000;
#ifdef PSX_TEST_REAL_STEP
  if(!lwc)psx_cycle_count--;
#endif
  effect_test=1;device_flag=1;effect_order=0;effect_deadline=1002;
#ifdef PSX_TEST_DEADLINE
  psx_cycle_count-=5;g_psx_cyc_batch=5;psx_next_service_cycle=effect_deadline;
#endif
  (void)perform_load(&c,width,lwc,0xbf801120);
  if(sample_cycle!=1001 || psx_cycle_count!=(lwc?1002u:1003u) || effect_order!=12 || device_flag!=1 || reads!=1){
   fprintf(stderr,"side-effect case %u: sample=%llu end=%llu order=%d flag=%d reads=%u\n",kind,(unsigned long long)sample_cycle,(unsigned long long)psx_cycle_count,effect_order,device_flag,reads);failures++;
  }
  effect_test=0;
 }
 puts("4 side-effect order cases checked: read/clear before completion event/set");return failures?1:0;
}
