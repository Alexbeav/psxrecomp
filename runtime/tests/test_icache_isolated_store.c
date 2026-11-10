/* Source-independent driver of the real store guard prefixes and cache timing.
 * The oracle vectors come from the unmodified original CPU WriteMemory method.
 * No BIOS instructions, ROM, or retail data is present in this fixture. */
#include "psx_icache.h"
#include "psx_cycles.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
uint64_t psx_cycle_count,psx_next_service_cycle;
int psx_in_device_service,g_ls_replay_active,g_event_step_conservative;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
uint64_t g_guest_store_count,g_kseg2_ignored_writes;
static uint32_t sr,cache_ctrl,*sr_ptr=&sr;
static unsigned ordinary_writes;
void psx_devices_service_to_now(void){}
void psx_advance_cycles_slow(uint32_t n){psx_cycle_count+=n;}
#include PSX_TEST_STORE_PREFIXES
static void write_width(unsigned w,uint32_t a){
 if(w==1)psx_write_byte_raw(a,0xa5);else if(w==2)psx_write_half_raw(a,0xa5a5);else psx_write_word_raw(a,0xa5a5a5a5);
}
int main(void){
 unsigned biu,status,width,addr,expected_hash,expected_writes,count=0,fail=0;
 while(scanf("%u %u %u %u %u %u",&biu,&status,&width,&addr,&expected_hash,&expected_writes)==6){
  psx_icache_reset();sr=0;psx_write_word_raw(0xfffe0130u,biu);
  for(unsigned i=0;i<1024;i++)g_psx_icache_tv[i]=0x2000+i*4;
  sr=status;ordinary_writes=0;write_width(width,addr);
  unsigned hash=2166136261u;for(unsigned i=0;i<1024;i++)hash=(hash^g_psx_icache_tv[i])*16777619u;
  if(hash!=expected_hash || ordinary_writes!=expected_writes){if(!fail)fprintf(stderr,"case %u biu%x sr%x width%u addr%x hash%x/%x writes%u/%u\n",count,biu,status,width,addr,hash,expected_hash,ordinary_writes,expected_writes);fail++;}
  if(status && (biu&0x804)==0x804){
   CPUState cpu;memset(&cpu,0,sizeof(cpu));cpu.read_absorb_which=3;cpu.read_absorb[3]=9;psx_cycle_count=0;
   psx_icache_fetch(&cpu,0x27a0);psx_icache_fetch_interp(&cpu,0x27b0);
   /* Only the selected 16-byte line was invalidated. Adjacent line stays warm. */
   if(psx_cycle_count!=7 || cpu.read_absorb[3] || cpu.read_absorb_which)fail++;
   psx_icache_fetch_interp(&cpu,0x27a0);if(psx_cycle_count!=7)fail++;
  }
  count++;
 }
 printf("%s %u source store cases, %u failed checks; cache refill and give-back clear\n",fail?"FAIL":"PASS",count,fail);return !!fail;
}
