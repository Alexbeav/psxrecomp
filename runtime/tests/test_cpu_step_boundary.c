/* Execute authored output from each real emitter against production objects.
 * Functional callback and instruction observer have independent enable flags.
 * Devices/retail traffic are excluded here; cold fetch and instruction effects
 * establish every generated boundary, including coalesced cached followers. */
#include "cpu_state.h"
#include "psx_icache.h"
#include "psx_cyc.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
extern void TEST_ENTRY(CPUState *);
extern CPUState *debug_cpu_ptr;
extern void memory_set_sr_ptr(const uint32_t *);
static uint32_t base,seen,trace_count;
static int uncached;
static void trace(uint32_t pc){(void)pc;trace_count++;}
static void step(CPUState *cpu,uint32_t pc,uint64_t cycle) {
    static const uint32_t values[6]={10,1,2,3,4,4};
    static const uint32_t cached_cycles[6]={0,8,9,10,11,19};
    assert(seen<6 && pc==base+4*seen && cpu->gpr[8]==values[seen]);
    assert(cycle==(uncached?5*seen:cached_cycles[seen]));
    printf("step %08X %llu %u\n",pc,(unsigned long long)cycle,cpu->gpr[8]);seen++;
}
int main(int argc,char **argv) {
    assert(argc==4);base=strtoul(argv[1],0,16);uncached=base>=0xa0000000u;
    int active=atoi(argv[2]),tracing=atoi(argv[3]);
    CPUState cpu={0};cpu.gpr[8]=10;cpu.gpr[31]=0x80090000;
    cpu.read_fudge=32;cpu.ld_which_t=32;debug_cpu_ptr=&cpu;
    memory_set_sr_ptr(&cpu.cop0[12]);psx_icache_reset();g_psx_icache_active=1;
    psx_next_service_cycle=1000000;
    g_input_instruction_histogram_active=tracing;g_input_instruction_histogram_callback=trace;
    g_psx_cpu_step_boundary_callback=active?step:0;
    TEST_ENTRY(&cpu);psx_cyc_batch_flush();
    assert(cpu.gpr[8]==4 && cpu.pc==cpu.gpr[31]);
    assert(psx_get_cycle_count()==(uncached?30:20));
    assert(seen==(active?6:0));assert(trace_count==(tracing?(uncached?6:2):0));
    printf("final %u %u %llu %u %u\n",cpu.gpr[8],cpu.pc,(unsigned long long)psx_get_cycle_count(),seen,trace_count);
    return 0;
}
