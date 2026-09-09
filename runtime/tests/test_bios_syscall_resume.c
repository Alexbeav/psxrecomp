#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_bios_backend.h"
#include "psx_icache.h"
#include "psx_cycles.h"
extern const PsxBiosBackend Test_psx_bios_backend;
extern void Test_psx_dispatch_call(CPUState*,uint32_t,uint32_t);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern void memory_set_sr_ptr(const uint32_t*);
extern CPUState *debug_cpu_ptr;
extern uint64_t g_dispatch_static_hits;
extern int g_input_instruction_histogram_active;
static unsigned char code[0x48];
static uint32_t fetches[32]; static unsigned nf;
static void sample(uint32_t pc) { if(nf<32) fetches[nf++]=pc; }
void __wrap_input_instruction_histogram_sample(uint32_t pc) { sample(pc); }
static uint32_t read_word(uint32_t addr) {
 uint32_t phys=addr&0x1fffffff,value;
 if(phys>=0x1fc00000 && phys+4<=0x1fc00000+sizeof(code)) {
  memcpy(&value,code+(phys-0x1fc00000),4);return value;
 }
 return psx_read_word(addr);
}
int main(int argc,char**argv) {
 if(argc!=3)return 2;
 FILE*f=fopen(argv[1],"rb");if(!f)return 3;
 if(fread(code,1,sizeof(code),f)!=sizeof(code)){fclose(f);return 4;}fclose(f);
 CPUState cpu={0};cpu.read_word=read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
 cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
 cpu.read_fudge=32;cpu.ld_which_t=32;cpu.gpr[29]=0x1ff000;cpu.gpr[31]=0x1000;cpu.gpr[16]=0x1000;cpu.gpr[4]=1;
 psx_bios_activate(&Test_psx_bios_backend);debug_cpu_ptr=&cpu;memory_set_sr_ptr(&cpu.cop0[12]);
 psx_icache_reset();g_psx_icache_active=1;g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=sample;psx_next_service_cycle=1000000;
 uint64_t entry_cycles=0;uint32_t resumed_pc=0;
 if(!strcmp(argv[2],"sys")) {
  Test_psx_dispatch_call(&cpu,0xbfc00000,0x80000080);psx_cyc_batch_flush();entry_cycles=psx_cycle_count;
  /* Supply the ordinary non-delay-slot EPC+4 return to isolate admission.
     This authored fixture does not simulate the guest handler or RFE. */
  resumed_pc=cpu.cop0[14]+4;
  Test_psx_dispatch_call(&cpu,resumed_pc,0x1000);
 } else Test_psx_dispatch_call(&cpu,0xbfc00020,0x1000);
 psx_cyc_batch_flush();
 printf("{\"entry_cycles\":%llu,\"cycles\":%llu,\"static_hits\":%llu,\"s1\":%u,\"epc\":%u,\"cause\":%u,\"resumed_pc\":%u,\"pc\":%u,\"fetches\":[",(unsigned long long)entry_cycles,(unsigned long long)psx_cycle_count,(unsigned long long)g_dispatch_static_hits,cpu.gpr[17],cpu.cop0[14],cpu.cop0[13],resumed_pc,cpu.pc);
 for(unsigned i=0;i<nf;i++)printf("%s%u",i?",":"",fetches[i]);puts("]}");return 0;
}
