/* Actual GPU reader, software renderer, DMA controller and deadline scheduler.
 * Source-observed authored GP0/register history; no CPU or retail input. */
#include "source_gpu_runtime.h"
#include "gpu.h"
#include "gpu_render.h"
#include "dma.h"
#include "timers.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
extern void memory_init(const char *);
extern uint8_t *memory_get_ram_ptr(void);
static void to(uint64_t time) {assert(time>=psx_get_cycle_count());psx_advance_cycles((uint32_t)(time-psx_get_cycle_count()));psx_devices_service_to_now();}
static void report(uint64_t sample) {
    SourceGPUServiceClock clock;SourceGPUCommandProjection s;source_gpu_runtime_copy(&clock,&s);
    printf("state %llu %u %u %u %u %llu %u %u %u %u\n",(unsigned long long)sample,(uint32_t)s.budget,s.phase,s.count,(unsigned)source_gpu_command_ready(&s),(unsigned long long)s.last_update,s.command,s.count?s.queue[0]:0,dma_read(0x1f8010a8),dma_read(0x1f8010f4));
}
int main(int argc,char **argv) {
    assert(argc==5);memory_init(argv[1]);timers_init();dma_init();interrupts_init();
    gr_init((uint16_t*)gpu_get_vram());gpu_init();source_gpu_runtime_init();
    FILE *ram=fopen(argv[2],"rb");assert(ram);assert(fread(memory_get_ram_ptr(),1,0x3000,ram)==0x3000);fclose(ram);
    gpu_write_gp0(0xe3000000);gpu_write_gp0(0xe407ffff);
    to(98);dma_write(0x1f8010f0,0x800);to(108);dma_write(0x1f8010f4,0x840000);
    to(120);dma_write(0x1f8010a0,0x1000);to(121);dma_write(0x1f8010a4,0);
    to(146);dma_write(0x1f8010a8,0x01000401);report(146);
    FILE *times=fopen(argv[3],"r");assert(times);unsigned long long time;
    while(fscanf(times,"%llu",&time)==1){assert(time>=146);to(time);report(time);}fclose(times);
    assert((dma_read(0x1f8010a8)&0x1000000u)==0);
    if(atoi(argv[4])) {
        unsigned n=atoi(argv[4]);(void)n;
        /* Renderer visibility is not source-qualified by this timing test. */
        assert(gpu_vram_peek(0,0)!=0);
    }
    return 0;
}
