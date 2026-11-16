#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_bios_backend.h"
extern const PsxBiosBackend Test_psx_bios_backend;
#include "psx_icache.h"
#include "psx_cycles.h"
extern unsigned char* memory_get_ram_ptr(void);
extern void dirty_ram_mark_executable_range(uint32_t,uint32_t);
extern void memory_set_sr_ptr(const uint32_t*);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern void Test_psx_dispatch_call(CPUState*,uint32_t,uint32_t);
extern uint64_t g_dispatch_static_hits;
extern CPUState *debug_cpu_ptr;
extern int g_input_instruction_histogram_active;
static uint32_t fetches[256]; static unsigned nf;
void __wrap_input_instruction_histogram_sample(uint32_t pc) { if(nf<256)fetches[nf++]=pc; }
int main(int argc,char**argv) {
 if(argc!=5)return 2;
 uint32_t alias=(uint32_t)strtoul(argv[1],0,16), entry=(uint32_t)strtoul(argv[2],0,16), stop=0x1000;
 FILE*f=fopen(argv[3],"rb"); if(!f)return 3;
 fread(memory_get_ram_ptr()+0x500,1,0x130,f);fclose(f);if(atoi(argv[4]))dirty_ram_mark_executable_range(0x500,0x130);
 CPUState cpu={0};cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
 cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
 cpu.read_fudge=32;cpu.ld_which_t=32;cpu.gpr[29]=0x1ff000;cpu.gpr[31]=0x1000;cpu.gpr[17]=0x1000;cpu.gpr[8]=99;cpu.gpr[4]=1;
 if(entry==0xa0){memcpy(memory_get_ram_ptr()+0xa0,memory_get_ram_ptr()+0x500,16);if(atoi(argv[4]))dirty_ram_mark_executable_range(0xa0,16);}
 if(entry==0x500 || entry==0xa0)stop=0x598;
 if(entry==0x5c0)stop=0x80000080;
 psx_bios_activate(&Test_psx_bios_backend);debug_cpu_ptr=&cpu;memory_set_sr_ptr(&cpu.cop0[12]);
 psx_icache_reset();g_psx_icache_active=1;g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=__wrap_input_instruction_histogram_sample;psx_next_service_cycle=1000000;
 Test_psx_dispatch_call(&cpu,alias|entry,stop);psx_cyc_batch_flush();
 printf("{\"alias\":%u,\"entry\":%u,\"cycles\":%llu,\"static_hits\":%llu,\"s0\":%u,\"v0\":%u,\"t0\":%u,\"epc\":%u,\"cause\":%u,\"pc\":%u,\"fetches\":[",alias,entry,(unsigned long long)psx_cycle_count,(unsigned long long)g_dispatch_static_hits,cpu.gpr[16],cpu.gpr[2],cpu.gpr[8],cpu.cop0[14],cpu.cop0[13],cpu.pc);
 for(unsigned i=0;i<nf;i++)printf("%s%u",i?",":"",fetches[i]);puts("]}");return 0;
}
