/* Production return observer with actual CPUState and inert unused diagnostics. */
#include "cpu_state.h"
#include <signal.h>
#include <stdlib.h>
uint32_t g_psx_icache_tv[1024];
int g_psx_icache_active,g_precise_mode,g_dirty_interp_active,g_interp_boundary_in_progress;
uint32_t i_stat,i_mask,g_slice_probe_pc,g_slice_probe_deadline,g_slice_probe_bound;
uint64_t g_slice_probe_cycle,g_slice_irq_taken;
static unsigned dma_cpu_read_penalty(void) { return 0; }
#include "source_cpu_boundary_probe.h"
/* These controls select CPU capture, not device history or a diagnostic window.
 * Pure empty providers satisfy the expanded passive-observer dependencies. */
uint64_t cdrom_debug_get_trace(const CDROMTraceEntry **out) { *out=NULL;return 0; }
void spu_get_global_state(SpuGlobalState *out) { memset(out,0,sizeof(*out)); }
uint64_t dma_debug_get_trace(const DMATraceEntry **out) { *out=NULL;return 0; }
void dma_debug_get_state(DMADebugState *out) { memset(out,0,sizeof(*out)); }
void source_gpu_runtime_copy_return(SourceGPUServiceClock *clock,SourceGPUCommandProjection *command) {
    memset(clock,0,sizeof(*clock));memset(command,0,sizeof(*command));
}
uint32_t mdec_snapshot_bytes(void) { abort(); }
void mdec_snapshot_write(uint8_t *out) { (void)out;abort(); }
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
