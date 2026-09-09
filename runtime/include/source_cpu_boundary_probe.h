#ifndef PSX_SOURCE_CPU_BOUNDARY_PROBE_H
#define PSX_SOURCE_CPU_BOUNDARY_PROBE_H
#include <stdio.h>
#include <stdlib.h>
#include "source_observer_limit.h"
/* One passive snapshot per original-model frontend return. Unlike input-route
 * fields, these boundaries include the CPU instruction's event overshoot. */
static void source_cpu_return_probe(CPUState *cpu,uint32_t pc,uint64_t cycle,unsigned frame) {
    static int initialized,enabled;
    static unsigned max_frames=20000;
    static FILE *stream;
    if(!initialized) {
        initialized=1;
        const char *setting=getenv("PSX_SOURCE_CPU_RETURN_PROBE");
        enabled=setting && setting[0]=='1' && !setting[1];
        if(enabled)max_frames=source_observer_max_frames("PSX_SOURCE_CPU_MAX_FRAMES");
    }
    if(!enabled)return;
    if(frame<1 || frame>max_frames)abort();
    if(!stream) {
        char path[4096];const char *directory=getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
        if(!directory || snprintf(path,sizeof(path),"%s/cpu-return.tsv",directory)>=(int)sizeof(path))abort();
        stream=fopen(path,"wx");if(!stream)abort();
        fprintf(stream,"frame\tpc\tcycle\tsr\tcause\tepc");
        for(unsigned i=0;i<32;i++)fprintf(stream,"\tr%u",i);
        fputc('\n',stream);
    }
    fprintf(stream,"%u\t%08X\t%llu\t%08X\t%08X\t%08X",frame,pc,(unsigned long long)cycle,cpu->cop0[12],cpu->cop0[13],cpu->cop0[14]);
    for(unsigned i=0;i<32;i++)fprintf(stream,"\t%08X",cpu->gpr[i]);
    fputc('\n',stream);fflush(stream);
}
/* Passive, bounded diagnostic at the existing functional pre-fetch boundary.
 * It reads CPU/cache scalars only; never reads guest memory or services devices. */
static void source_cpu_boundary_probe(CPUState *cpu,uint32_t pc,uint64_t cycle) {
    static int initialized;
    static unsigned long long low,high;
    static unsigned rows;
    static FILE *stream;
    if(!initialized) {
        initialized=1;
        const char *range=getenv("PSX_SOURCE_CPU_BOUNDARY_WINDOW");
        if(range && (sscanf(range,"%llu,%llu",&low,&high)!=2 || high<=low || high-low>1000000u))abort();
    }
    if(!high || cycle<low)return;
    if(cycle>high) {if(stream){fclose(stream);stream=0;}high=0;return;}
    if(!stream) {
        char path[4096];const char *directory=getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
        if(!directory || snprintf(path,sizeof(path),"%s/cpu-boundary.tsv",directory)>=(int)sizeof(path))abort();
        stream=fopen(path,"wx");if(!stream)abort();
        fprintf(stream,"pc\tcycle\tcache_tag\tcache_active\tsr\tcause\tepc");
        for(unsigned i=0;i<32;i++)fprintf(stream,"\tr%u",i);
        fprintf(stream,"\tread_fudge\tld_absorb\tld_which\tread_absorb_which\tdma_read_wait\tprecise\tdirty\ti_stat\ti_mask\tslice_pc\tslice_deadline\tslice_bound\tslice_cycle\tslice_takes\n");
    }
    if(++rows>500000u)abort();
    fprintf(stream,"%08X\t%llu\t%08X\t%d\t%08X\t%08X\t%08X",pc,(unsigned long long)cycle,g_psx_icache_tv[(pc&0xffcu)>>2],g_psx_icache_active,cpu->cop0[12],cpu->cop0[13],cpu->cop0[14]);
    for(unsigned i=0;i<32;i++)fprintf(stream,"\t%08X",cpu->gpr[i]);
    extern int g_precise_mode,g_dirty_interp_active;
    extern uint32_t i_stat,i_mask,g_slice_probe_pc,g_slice_probe_deadline,g_slice_probe_bound;
    extern uint64_t g_slice_probe_cycle,g_slice_irq_taken;
    fprintf(stream,"\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%08X\t%08X\t%08X\t%u\t%u\t%llu\t%llu\n",cpu->read_fudge,cpu->ld_absorb,cpu->ld_which_t,cpu->read_absorb_which,dma_cpu_read_penalty(),g_precise_mode,g_dirty_interp_active,i_stat,i_mask,g_slice_probe_pc,g_slice_probe_deadline,g_slice_probe_bound,(unsigned long long)g_slice_probe_cycle,(unsigned long long)g_slice_irq_taken);
}
#endif
