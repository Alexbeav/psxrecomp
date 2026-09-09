/* Source-authored, explicitly timed ordinary setup; production GPU/DMA service. */
#include "source_gpu_runtime.h"
#include "gpu.h"
#include "gpu_render.h"
#include "dma.h"
#include "timers.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include <assert.h>
#include <stdio.h>
extern void memory_init(const char *);
extern uint8_t *memory_get_ram_ptr(void);
static void to(uint64_t t){assert(t>=psx_get_cycle_count());psx_advance_cycles((uint32_t)(t-psx_get_cycle_count()));psx_devices_service_to_now();}
int main(int argc,char **argv){
 assert(argc==4);memory_init(argv[1]);timers_init();dma_init();interrupts_init();gr_init((uint16_t*)gpu_get_vram());gpu_init();source_gpu_runtime_init();
 FILE *f=fopen(argv[2],"rb");assert(f);assert(fread(memory_get_ram_ptr(),1,0x3000,f)==0x3000);fclose(f);
 f=fopen(argv[3],"r");assert(f);unsigned long long t;unsigned addr,word;char op;int result=0;
 while(fscanf(f," %c %llu %x %x",&op,&t,&addr,&word)==4){
  to(t);
  if(op=='W'){
   if(addr==0x1f801810)gpu_write_gp0(word);
   else if(addr==0x1f801814)gpu_write_gp1(word);
   else dma_write(addr,word);
  }else if(op=='R'){
   uint32_t actual=gpu_read_gpustat();
   printf("register %llu %08x %08x %08x\n",t,addr,word,actual);
   if((actual&addr)!=(word&addr))result=3;
  }else if(op=='V'){
   char path[64];snprintf(path,sizeof(path),"vram-%u.bin",addr);
   FILE *dump=fopen(path,"wb");assert(dump);
   assert(fwrite(gpu_get_vram(),1,1024u*512u*2u,dump)==1024u*512u*2u);
   assert(fclose(dump)==0);
  }else{
   SourceGPUCommandProjection s;source_gpu_runtime_copy(0,&s);
   printf("state %llu %u %u %u %u %llu %u %u\n",t,(uint32_t)s.budget,s.phase,s.count,(unsigned)source_gpu_command_ready(&s),(unsigned long long)s.last_update,s.command,s.count?s.queue[0]:0);
  }
 }
 fclose(f);return result;
}
