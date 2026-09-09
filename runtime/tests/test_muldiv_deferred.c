#include "psx_cyc.h"
#include <assert.h>
#include <stdio.h>
uint64_t psx_cycle_count,psx_next_service_cycle=UINT64_MAX;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
int g_psx_cyc_bb_defer,psx_in_device_service,g_event_step_conservative,g_ls_replay_active;
void psx_devices_service_to_now(void){assert(0);}
void psx_advance_cycles_slow(uint32_t n){psx_cycle_count+=n;}
#include PSX_TEST_MULDIV_INCLUDE
static uint32_t local;
static void reset(uint64_t clock,unsigned batch,unsigned local_value) {
 psx_cycle_count=clock;g_psx_cyc_batch=batch;local=local_value;
 g_psx_cyc_local_acc=local_value?&local:NULL;g_psx_cyc_bb_defer=1;
}
int main(int argc,char **argv) {
 assert(argc==2);CPUState cpu={0};
 /* Authored sequence mirrors the measured shift/MULT/store/MFLO timing.
  * The preceding load commits on shift; MULT consumes its give-back.
  * Source timestamps give 8 cycles before the next instruction fetch. */
 cpu.ld_which_t=3;cpu.ld_absorb=5;reset(1000,0,0);
 psx_cyc_step(&cpu,1u<<2);psx_cyc_step(&cpu,(1u<<2)|(1u<<3));
 psx_muldiv_set(&cpu,7);
 psx_cyc_step(&cpu,(1u<<17)|(1u<<4));psx_cyc_step(&cpu,1u<<6);
 psx_muldiv_stall(&cpu);psx_cyc_batch_flush();
 if(psx_cycle_count!=1008){printf("FAIL measured synthetic span: expected1008 actual%llu\n",(unsigned long long)psx_cycle_count);return 1;}
 for(unsigned pending=0;pending<64;pending++)for(unsigned mode=0;mode<3;mode++) {
  reset(1000,mode==1?0:pending,mode==0?0:pending);psx_muldiv_set(&cpu,37);
  assert(cpu.muldiv_ts_done==1037+(mode==2?2:1)*pending);
 }
 FILE *f=fopen(argv[1],"r");assert(f);unsigned latency,elapsed,give,want_give;unsigned long long want_clock,want_deadline;unsigned cases=0;
 while(fscanf(f,"%u %u %u %llu %llu %u",&latency,&elapsed,&give,&want_clock,&want_deadline,&want_give)==6) {
  for(unsigned pending=0;pending<=elapsed;pending++)for(unsigned mode=0;mode<2;mode++) {
   memset(&cpu,0,sizeof(cpu));cpu.read_absorb_which=3;cpu.read_absorb[3]=give;cpu.muldiv_ts_done=1000+latency;
   reset(1000+elapsed-pending,mode?0:pending,mode?pending:0);
   psx_muldiv_stall(&cpu);psx_cyc_batch_flush();
   assert(psx_cycle_count==want_clock && cpu.muldiv_ts_done==want_deadline && cpu.read_absorb[3]==want_give);
   assert(g_psx_cyc_batch==0 && local==0);cases++;
  }
 }
 assert(feof(f));fclose(f);assert(cases==25944);
 puts("PASS multiply deadline, measured load/MULT/read span, and 25944 exact-source stall/deferred cases");
}
