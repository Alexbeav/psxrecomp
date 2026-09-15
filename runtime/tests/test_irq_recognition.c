#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_bios_backend.h"
#include "psx_icache.h"
#include "psx_cycles.h"
#include "interrupts.h"
#include "timers.h"
extern const PsxBiosBackend Test_psx_bios_backend;
extern void Test_psx_dispatch_call(CPUState*,uint32_t,uint32_t);
extern unsigned char* memory_get_ram_ptr(void);
extern void dirty_ram_mark_executable_range(uint32_t,uint32_t);
extern void memory_set_sr_ptr(const uint32_t*);
extern void psx_irq_set_cause_ptr(uint32_t*);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern CPUState *debug_cpu_ptr;
extern uint32_t i_stat,i_mask;
extern uint64_t g_dispatch_static_hits;
extern uint64_t g_irq_deliver_count,g_slice_irq_taken;
#ifndef AUTHORED_LOAD_RETURN_ONLY
extern void Test_func_1FC00034(CPUState*);
#endif
static CPUState *active;static unsigned count,stores;
typedef struct {uint32_t pc,sr,cause,epc,t8,t9,word,v0,ra,sp;uint64_t cycle;} Event;
static Event events[128];
void __wrap_input_instruction_histogram_sample(uint32_t pc) {
 if(count<128)events[count++]=(Event){pc,active->cop0[12],active->cop0[13],active->cop0[14],active->gpr[24],active->gpr[25],psx_read_word(0x1000),active->gpr[2],active->gpr[31],active->gpr[29],psx_get_cycle_count()};
}
static void write_word(uint32_t a,uint32_t v){if((a&0x1fffffff)==0x1000)stores++;psx_write_word(a,v);}
int main(int argc,char**argv) {
 if(argc!=5)return 2;unsigned target=atoi(argv[2]),irq=atoi(argv[3]),masked=atoi(argv[4]);
 unsigned char*ram=memory_get_ram_ptr();FILE*f=fopen(argv[1],"rb");if(!f)return 3;
 fread(ram+0x500,1,0x3c,f);fclose(f);
 uint32_t handler[]={0x401a7000,0x3c1b1f80,0xa7601070,0x03400008,0x42000010};
 memcpy(ram+0x80,handler,sizeof(handler));psx_write_word(0x1000,10);
 dirty_ram_mark_executable_range(0x80,0x20);
 CPUState c={0};active=&c;c.read_word=psx_read_word;c.read_half=psx_read_half;c.read_byte=psx_read_byte;c.write_word=write_word;c.write_half=psx_write_half;c.write_byte=psx_write_byte;
 c.read_fudge=32;c.ld_which_t=32;c.gpr[9]=target;c.gpr[11]=0x1f801120;c.gpr[12]=irq?0x18:8;c.gpr[16]=0x1000;c.gpr[29]=0x1ff000;c.gpr[31]=0xbfc00200;c.cop0[12]=0x401;
 if(getenv("AUTHORED_LOAD_RETURN")){
  c.gpr[31]=0x80000524;c.gpr[19]=0xbfc00200;c.cop0[12]=0;
  psx_write_word(0x1feffc,0xbfc00200);
 }
 psx_bios_activate(&Test_psx_bios_backend);debug_cpu_ptr=&c;memory_set_sr_ptr(&c.cop0[12]);psx_irq_set_cause_ptr(&c.cop0[13]);psx_icache_reset();g_psx_icache_active=1;timers_init();i_mask=masked?0:0x40;i_stat=0;
 g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=__wrap_input_instruction_histogram_sample;psx_next_service_cycle=0;
 const char*slice=getenv("AUTHORED_TEST_SLICE");
 if(slice && slice[0]=='1') {
  g_psx_precise_slice=1;
  if(!psx_slice_block_impl(&c,0x80000500,14,1))return 6;
  Test_psx_dispatch_call(&c,c.pc,0xbfc00200);
 } else {
  if(slice && slice[0]=='2')g_psx_precise_slice=1;
  Test_psx_dispatch_call(&c,0x80000500,0xbfc00200);
 }
 psx_cyc_batch_flush();
#ifndef AUTHORED_LOAD_RETURN_ONLY
 if(getenv("AUTHORED_COOLDOWN_PROBE")) {
  uint64_t before=psx_get_cycle_count(),delivered=g_irq_deliver_count,taken=g_slice_irq_taken;
  unsigned s1=c.gpr[17];
  int initial_cooldown=0;psx_get_freeze_diag(NULL,NULL,NULL,&initial_cooldown,NULL,NULL);
  i_mask|=8;psx_irq_raise(IRQ_DMA,0);c.pc=0;g_psx_precise_slice=1;
  Test_func_1FC00034(&c);
  psx_cyc_batch_flush();
  fprintf(stderr,"cooldown-probe before=%llu after=%llu actual=%llu reported=%llu pc=%08X slot=%u\n",(unsigned long long)before,(unsigned long long)psx_get_cycle_count(),(unsigned long long)(g_irq_deliver_count-delivered),(unsigned long long)(g_slice_irq_taken-taken),c.pc,c.gpr[17]-s1);
  if(initial_cooldown>0){
   for(unsigned j=0;j<3;j++){c.pc=0;Test_func_1FC00034(&c);psx_cyc_batch_flush();}
   int remaining=0;psx_get_freeze_diag(NULL,NULL,NULL,&remaining,NULL,NULL);
   fprintf(stderr,"cooldown-repeat cycles=%llu slots=%u actual=%llu reported=%llu remaining=%d\n",(unsigned long long)(psx_get_cycle_count()-before),c.gpr[17]-s1,(unsigned long long)(g_irq_deliver_count-delivered),(unsigned long long)(g_slice_irq_taken-taken),remaining);
   if(remaining>0){
    psx_advance_cycles((uint32_t)remaining-1u);psx_cyc_batch_flush();
    psx_check_interrupts_at(&c,0x80000534);
    uint64_t below=g_irq_deliver_count-delivered;
    psx_advance_cycles(1);psx_cyc_batch_flush();
    psx_check_interrupts_at(&c,0x80000534);
    fprintf(stderr,"cooldown-expiry below=%llu at=%llu pending=%u epc=%08X\n",(unsigned long long)below,(unsigned long long)(g_irq_deliver_count-delivered),i_stat&i_mask,c.cop0[14]);
   }
  }
 }
#endif
 fprintf(stderr,"istat=%08X mask=%08X count=%u mode=%X target=%u next=%u\n",i_stat,i_mask,timers_read(0x1f801120),timers_read(0x1f801124),timers_read(0x1f801128),timers_cycles_to_irq(~0u));
 printf("{\"cycles\":%llu,\"memory\":%u,\"stores\":%u,\"s0\":%u,\"s1\":%u,\"s2\":%u,\"ra\":%u,\"sp\":%u,\"t9\":%u,\"epc\":%u,\"sr\":%u,\"pc\":%u,\"static_hits\":%llu,\"events\":[",(unsigned long long)psx_cycle_count,psx_read_word(0x1000),stores,c.gpr[16],c.gpr[17],c.gpr[18],c.gpr[31],c.gpr[29],c.gpr[25],c.cop0[14],c.cop0[12],c.pc,(unsigned long long)g_dispatch_static_hits);
 for(unsigned i=0;i<count;i++){Event*e=&events[i];printf("%s{\"pc\":%u,\"cycle\":%llu,\"sr\":%u,\"cause\":%u,\"epc\":%u,\"t8\":%u,\"t9\":%u,\"memory\":%u,\"v0\":%u,\"ra\":%u,\"sp\":%u}",i?",":"",e->pc,(unsigned long long)e->cycle,e->sr,e->cause,e->epc,e->t8,e->t9,e->word,e->v0,e->ra,e->sp);}puts("]}");return 0;
}
