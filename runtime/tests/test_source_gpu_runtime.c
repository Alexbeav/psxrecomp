/* Actual device scheduler and CPU paths. No game, rendering, or DMA payload
 * admission change; GP0/DMA-write inputs are retained authored source events. */
#include "../src/dirty_ram_interp.c"
#include "source_gpu_runtime.h"
#include "timers.h"
#include "dma.h"
#include "gpu.h"
#include "gpu_render.h"
#include <assert.h>
#ifdef TEST_ENTRY
extern void TEST_ENTRY(CPUState *);
#endif
extern void memory_init(const char *);
extern void memory_set_sr_ptr(const uint32_t *);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern CPUState *debug_cpu_ptr;
static unsigned traces;
static void trace(uint32_t pc){(void)pc;traces++;}
static void to(uint64_t time) {assert(time>=psx_get_cycle_count());psx_advance_cycles((uint32_t)(time-psx_get_cycle_count()));psx_devices_service_to_now();}
int main(int argc,char **argv) {
    assert(argc==5);memory_init(argv[1]);timers_init();dma_init();interrupts_init();
    uint32_t base=strtoul(argv[2],0,16);int tracing=atoi(argv[3]),active=atoi(argv[4]);
    CPUState cpu={0};cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
    cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
    cpu.read_fudge=32;cpu.ld_which_t=32;cpu.gpr[8]=10;cpu.gpr[31]=0x80090000;
    debug_cpu_ptr=&cpu;memory_set_sr_ptr(&cpu.cop0[12]);psx_icache_reset();g_psx_icache_active=1;
    if(active)source_gpu_runtime_init();
#ifdef TEST_DMA
    gr_init((uint16_t*)gpu_get_vram());gpu_init();
    const uint32_t node1[]={0x05002000,0x280000ff,0,32,32u<<16,32u|(32u<<16)};
    const uint32_t node2[]={0x06ffffff,0,0,0,0,0,0};
    memcpy(memory_get_ram_ptr()+0x1000,node1,sizeof(node1));memcpy(memory_get_ram_ptr()+0x2000,node2,sizeof(node2));
    gpu_write_gp0(0xe3000000);gpu_write_gp0(0xe407ffff);
    to(98);dma_write(0x1f8010f0,0x800);to(108);dma_write(0x1f8010f4,0x840000);
    to(120);dma_write(0x1f8010a0,0x1000);to(121);dma_write(0x1f8010a4,0);
    to(1117005);dma_write(0x1f8010a8,0x01000401);
#else
    source_gpu_runtime_gp0(0xe3000000);source_gpu_runtime_gp0(0xe407ffff);
    const uint64_t writes[]={98,108,120,121,1117005};
    for(unsigned i=0;i<5;i++){to(writes[i]);source_gpu_runtime_dma_write();}
    const uint32_t quad[]={0x280000ff,0,32,32u<<16,32u|(32u<<16)};
    for(unsigned i=0;i<5;i++)source_gpu_runtime_gp0(quad[i]);
#endif
    SourceGPUCommandProjection command;SourceGPUServiceClock clock;
    source_gpu_runtime_copy(&clock,&command);
    assert(!active || (command.budget==-356 && command.phase==2 && command.count==1));
    int uncached=base>=0xa0000000u;to(uncached?1117173:1117170);
    /* Keep a real, previously cached scheduler deadline across the fetch. */
    assert(psx_next_service_cycle>psx_cycle_count);
    g_input_instruction_histogram_active=tracing;g_input_instruction_histogram_callback=trace;
#ifdef TEST_ENTRY
    TEST_ENTRY(&cpu);
#else
    const uint32_t words[]={0x24080001,0x25080001,0x25080001,0x25080001,0x03e00008,0};
    memcpy(memory_get_ram_ptr()+(base&0x1fffff),words,sizeof(words));
    g_precise_mode=1;g_dirty_interp_active=1;
    uint32_t pc=base;
    for(unsigned i=0;i<5;i++){uint32_t next=0;cpu.pc=pc;int moved=exec_one(&cpu,pc,&next);pc=moved?cpu.pc:next;}
    cpu.pc=pc;
#endif
    psx_cyc_batch_flush();
    assert(cpu.gpr[8]==4 && cpu.pc==cpu.gpr[31]);
    assert(psx_get_cycle_count()==(uncached?1117203:1117190));
    assert(tracing?traces>0:traces==0);
#ifdef TEST_DMA
    printf("dma_after_cpu %llu %08X %08X\n",(unsigned long long)psx_get_cycle_count(),dma_read(0x1f8010a8),dma_read(0x1f8010f4));
#endif
    source_gpu_runtime_copy_return(&clock,&command);
    printf("return %llu %llu %u %u %u %u %u\n",(unsigned long long)clock.cycle,(unsigned long long)clock.frame_request_cycle,clock.frame_returns,(uint32_t)command.budget,command.phase,command.count,(unsigned)source_gpu_command_ready(&command));
    if(active) {
        assert(clock.cycle==1117178 && clock.frame_request_cycle==1117175 && clock.frame_returns==1);
        assert(command.budget==-10 && command.phase==2 && command.count==1 && !source_gpu_command_ready(&command));
        source_gpu_runtime_copy(&clock,&command);assert(command.second_triangles==1);
        unsigned phases=command.first_triangles+command.second_triangles;
        psx_cpu_step_boundary(&cpu,cpu.pc);source_gpu_runtime_copy(&clock,&command);
        assert(clock.frame_returns==1 && phases==command.first_triangles+command.second_triangles);
    }else assert(clock.frame_returns==0);
    to(1117800);source_gpu_runtime_copy(&clock,&command);
#ifdef TEST_DMA
    assert(dma_read(0x1f8010a8)==0x401 && (dma_read(0x1f8010f4)&0x84000000u)==0x84000000u);
#endif
    assert(!active || (command.second_triangles==1 && command.budget==256 && source_gpu_command_ready(&command)));
    printf("final %u %08X %u %u %u\n",cpu.gpr[8],cpu.pc,traces,command.first_triangles,command.second_triangles);
    return 0;
}
