/* Production return observer with actual CPUState and inert unused diagnostics. */
#include "cpu_state.h"
#include <signal.h>
#include <stdlib.h>
uint32_t g_psx_icache_tv[1024];
int g_psx_icache_active,g_precise_mode,g_dirty_interp_active;
uint32_t i_stat,i_mask,g_slice_probe_pc,g_slice_probe_deadline,g_slice_probe_bound;
uint64_t g_slice_probe_cycle,g_slice_irq_taken;
static unsigned dma_cpu_read_penalty(void) { return 0; }
#include "source_cpu_boundary_probe.h"
static void expected_abort(int ignored) { (void)ignored;exit(4); }
int main(int argc,char **argv) {
    signal(SIGABRT,expected_abort);
    if(argc!=2)return 2;
    CPUState cpu={0};
    for(unsigned i=0;i<32;++i)cpu.gpr[i]=0x81000000u+i;
    cpu.cop0[12]=0x1234;cpu.cop0[13]=0x5678;cpu.cop0[14]=0x9abc;
    source_cpu_boundary_probe(&cpu,0,0); /* inactive by default */
    source_cpu_return_probe(&cpu,0x80012340,UINT64_C(40500000000),(unsigned)strtoul(argv[1],0,10));
    return 0;
}
