/* Production IRQ poll, real authored guest handler and return, private object
 * linkage. Source oracle uses independent original-core guest JR/RFE setup.
 * Direct poll starts at the same committed target boundary with no pending load.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_bios_backend.h"
#include "psx_icache.h"
#include "psx_cycles.h"
#include "interrupts.h"
extern unsigned char* memory_get_ram_ptr(void);
extern void dirty_ram_mark_executable_range(uint32_t,uint32_t);
extern int dirty_ram_dispatch(CPUState*,uint32_t,uint32_t);
extern void memory_set_sr_ptr(const uint32_t*);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern void psx_rfe_escape_check(CPUState*);
extern CPUState *debug_cpu_ptr;
extern uint32_t i_stat,i_mask;
static CPUState *active;static unsigned count,stores;
typedef struct {uint32_t pc,sr,cause,epc,r8,word,tag;uint64_t cycle;} Event;
static Event events[128];
void __wrap_input_instruction_histogram_sample(uint32_t pc) {
 if(count<128)events[count++]=(Event){pc,active->cop0[12],active->cop0[13],active->cop0[14],active->gpr[8],psx_read_word(0x1000),g_psx_icache_tv[(0x508&0xffc)>>2],psx_get_cycle_count()};
}
static void write_word(uint32_t a,uint32_t v){if((a&0x1fffffff)==0x1000)stores++;psx_write_word(a,v);}
static void dispatch_call(CPUState*c,uint32_t pc,uint32_t stop){
 for(unsigned i=0;i<100;i++){
  if(pc==stop||pc==0xbfc00200u){c->pc=pc;return;}
  c->pc=pc;
  psx_check_interrupts_at(c,pc);
  if(!dirty_ram_dispatch(c,pc,stop)){fprintf(stderr,"unhandled %08X\n",pc);exit(4);}
  psx_rfe_escape_check(c);pc=c->pc;
  if(!pc){fprintf(stderr,"null return\n");exit(5);}
 }
 fprintf(stderr,"dispatch bound\n");exit(6);
}
static void dispatch(CPUState*c,uint32_t pc){dispatch_call(c,pc,c->cop0[14]);}
static const PsxBiosImageInfo info={.image_id="AUTHORED-IRQ",.image_sha256="authored"};
static const PsxBiosBackend backend={&info,dispatch,dispatch_call,0,0};
int main(int argc,char**argv){
 if(argc!=6)return 2;uint32_t alias=strtoul(argv[1],0,16);int warm=atoi(argv[2]),irq=atoi(argv[3]),setup=atoi(argv[5]);
 FILE*f=fopen(argv[4],"rb");if(!f)return 3;fread(memory_get_ram_ptr(),1,0x2000,f);fclose(f);dirty_ram_mark_executable_range(0,0x2000);
 CPUState c={0};active=&c;c.read_word=psx_read_word;c.read_half=psx_read_half;c.read_byte=psx_read_byte;c.write_word=write_word;c.write_half=psx_write_half;c.write_byte=psx_write_byte;
 c.read_fudge=32;c.ld_which_t=32;c.gpr[8]=10;c.gpr[16]=0x1000;c.gpr[29]=0x1ff000;c.cop0[12]=0x101;c.cop0[13]=irq?0x100:0;c.pc=alias|0x508;
 if(irq==2)c.cop0[12]=1; /* pending IP0, IM0 masked */
 if(irq==3)c.cop0[12]=0x100; /* pending/enabled IP0, IEc clear */
 c.gpr[19]=0x1004;c.gpr[27]=alias|0x508;
 if(setup){c.cop0[12]=0x600000;c.cop0[13]=0;}
 psx_bios_activate(&backend);debug_cpu_ptr=&c;memory_set_sr_ptr(&c.cop0[12]);psx_icache_reset();g_psx_icache_active=1;
 if(warm && alias<0xa0000000u)for(unsigned i=0;i<4;i++)g_psx_icache_tv[0x500/4+i]=(alias|0x500)+4*i;
 g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=__wrap_input_instruction_histogram_sample;psx_next_service_cycle=1000000;
 dispatch_call(&c,setup?0x80000600u:alias|0x508,0xbfc00200);psx_cyc_batch_flush();
 printf("{\"cycles\":%llu,\"r8\":%u,\"memory\":%u,\"stores\":%u,\"epc\":%u,\"sr\":%u,\"cause\":%u,\"pc\":%u,\"events\":[",(unsigned long long)psx_cycle_count,c.gpr[8],psx_read_word(0x1000),stores,c.cop0[14],c.cop0[12],c.cop0[13],c.pc);
 for(unsigned i=0;i<count;i++){Event*e=&events[i];printf("%s{\"pc\":%u,\"cycle\":%llu,\"sr\":%u,\"cause\":%u,\"epc\":%u,\"r8\":%u,\"memory\":%u,\"target_tag\":%u}",i?",":"",e->pc,(unsigned long long)e->cycle,e->sr,e->cause,e->epc,e->r8,e->word,e->tag);}puts("]}");return 0;
}
