#ifndef PSX_SOURCE_CPU_BOUNDARY_PROBE_H
#define PSX_SOURCE_CPU_BOUNDARY_PROBE_H
#include <stdio.h>
#include <stdlib.h>
#include "source_observer_limit.h"
#include "psx_file_create_new.h"
/* Private diagnostic: copy existing immutable device-ring entries only.
 * No MMIO access, device service, ring clear or guest state write. */
#include "cdrom.h"
#include "spu.h"
#include "dma.h"
#include "mdec.h"
#include "source_gpu_runtime.h"
static unsigned source_device_history_frame;
static void source_device_history_probe(unsigned frame) {
    static int enabled=-1;static FILE *out,*spu_out,*dma_out;static uint64_t last,last_dma;
    if(enabled<0){const char *v=getenv("PSX_SOURCE_DEVICE_HISTORY_PROBE");enabled=v&&v[0]=='1'&&!v[1];}
    if(!enabled)return;
    if(!out){
        char path[4096];const char *dir=getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
        if(!dir||snprintf(path,sizeof(path),"%s/cd-history.tsv",dir)>=(int)sizeof(path))abort();
        out=psx_file_create_new(path,0);if(!out)abort();
        fputs("return\tseq\tcycle\tkind\taddr\tval\tpc\ti_stat\tindex\tirq_enable\tirq_flag\tpending_cmd\tpending_delay\n",out);
        if(snprintf(path,sizeof(path),"%s/spu-boundary.tsv",dir)>=(int)sizeof(path))abort();
        spu_out=psx_file_create_new(path,0);if(!spu_out)abort();
        fputs("return\tctrl\tcapture_pos\tirq_flag\n",spu_out);
        if(snprintf(path,sizeof(path),"%s/dma-history.tsv",dir)>=(int)sizeof(path))abort();
        dma_out=psx_file_create_new(path,0);if(!dma_out)abort();
        fputs("return\tseq\tcycle\tkind\tchannel\twords\taddr\tval\tmask\tmadr\tbcr\tchcr\tdpcr\tdicr_before\tdicr_after\ti_stat_before\ti_stat_after\tfunc\tstore_pc\n",dma_out);
    }
    const DMATraceEntry *dma_entries;uint64_t dma_end=dma_debug_get_trace(&dma_entries);
    if(dma_end<last_dma||dma_end-last_dma>DMA_TRACE_CAP)abort();
    for(;last_dma<dma_end;last_dma++){
        const DMATraceEntry *e=&dma_entries[last_dma%DMA_TRACE_CAP];
        if(e->seq!=last_dma)abort();
        fprintf(dma_out,"%u\t%llu\t%llu\t%c\t%u\t%u\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\n",frame,(unsigned long long)e->seq,(unsigned long long)e->cycle,e->kind,e->channel,e->total_words,e->addr,e->val,e->mask,e->madr,e->bcr,e->chcr,e->dpcr,e->dicr_before,e->dicr_after,e->i_stat_before,e->i_stat_after,e->func,e->pc);
    }
    fflush(dma_out);
    const CDROMTraceEntry *entries;uint64_t end=cdrom_debug_get_trace(&entries);
    if(end<last||end-last>CDROM_TRACE_CAP){fprintf(out,"RING_OVERFLOW last=%llu end=%llu\n",(unsigned long long)last,(unsigned long long)end);fflush(out);abort();}
    for(;last<end;last++){
        const CDROMTraceEntry *e=&entries[last%CDROM_TRACE_CAP];
        if(e->seq!=last)abort();
        /* Index writes/read polling remain in seq accounting, not the file.
         * Every command, parameter, IRQ/response/data transition is retained. */
        static uint32_t read_val[16];static unsigned read_seen;
        unsigned key=(e->index_reg&3u)*4u+(e->addr&3u);
        if(e->kind=='W'&&e->addr==0x1f801800u)continue;
        if(e->kind=='R'){
            if((read_seen&(1u<<key))&&read_val[key]==e->val)continue;
            read_seen|=1u<<key;read_val[key]=e->val;
        }
        fprintf(out,"%u\t%llu\t%llu\t%c\t%08X\t%08X\t%08X\t%08X\t%u\t%u\t%u\t%u\t%d\n",frame,(unsigned long long)e->seq,(unsigned long long)e->cycle,e->kind,e->addr,e->val,e->pc,e->i_stat,e->index_reg,e->irq_enable,e->irq_flag,e->pending_cmd,e->pending_delay);
    }
    SpuGlobalState spu;spu_get_global_state(&spu);
    fprintf(spu_out,"%u\t%04X\t%u\t%u\n",frame,spu.ctrl,spu.capture_pos,spu.irq_flag);
    fflush(out);fflush(spu_out);
}
/* One passive snapshot per original-model frontend return. Unlike input-route
 * fields, these boundaries include the CPU instruction's event overshoot. */
static void source_cpu_return_probe(CPUState *cpu,uint32_t pc,uint64_t cycle,unsigned frame) {
    static int initialized,enabled;
    static unsigned max_frames=20000;
    static FILE *stream,*extended,*gpu_stream,*dma_state,*pipeline,*mdec_states;
    if(!initialized) {
        initialized=1;
        const char *setting=getenv("PSX_SOURCE_CPU_RETURN_PROBE");
        enabled=setting && setting[0]=='1' && !setting[1];
        if(enabled)max_frames=source_observer_max_frames("PSX_SOURCE_CPU_MAX_FRAMES");
    }
    if(!enabled)return;
    source_device_history_frame=frame;
    source_device_history_probe(frame);
    if(frame<1 || frame>max_frames)abort();
    if(!stream) {
        char path[4096];const char *directory=getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
        if(!directory || snprintf(path,sizeof(path),"%s/cpu-return.tsv",directory)>=(int)sizeof(path))abort();
        stream=psx_file_create_new(path,0);if(!stream)abort();
        fprintf(stream,"frame\tpc\tcycle\tsr\tcause\tepc");
        for(unsigned i=0;i<32;i++)fprintf(stream,"\tr%u",i);
        fputc('\n',stream);
        if(snprintf(path,sizeof(path),"%s/cpu-extended.tsv",directory)>=(int)sizeof(path))abort();
        extended=psx_file_create_new(path,0);if(!extended)abort();
        fputs("frame\thi\tlo\n",extended);
        if(snprintf(path,sizeof(path),"%s/gpu-clock.tsv",directory)>=(int)sizeof(path))abort();
        gpu_stream=psx_file_create_new(path,0);if(!gpu_stream)abort();
        fputs("frame\tservice_cycle\tgpu_due\tdma_due\tframe_request\traster_cycle\tfraction\tremaining\tline_phase\tscanline\tlines\tfield\tmode\tstart\tend\tblank\ty_start\ty_offset\treadout_y\treadout_field\tbudget\tphase\tqueued\tdisplay_mode\tdma_direction\n",gpu_stream);
        if(snprintf(path,sizeof(path),"%s/dma-boundary.tsv",directory)>=(int)sizeof(path))abort();
        dma_state=psx_file_create_new(path,0);if(!dma_state)abort();
        fputs("frame\tcycle\tdpcr\tdicr\tchannel\tmadr\tbcr\tchcr\tactive\tremaining_words\tcycles_accum\n",dma_state);
        if(snprintf(path,sizeof(path),"%s/cpu-pipeline.tsv",directory)>=(int)sizeof(path))abort();
        pipeline=psx_file_create_new(path,0);if(!pipeline)abort();
        fputs("frame\tread_fudge\tld_absorb\tld_which\tread_absorb_which\tread_absorb_hex\n",pipeline);

    }
    fprintf(extended,"%u\t%08X\t%08X\n",frame,cpu->hi,cpu->lo);fflush(extended);
    fprintf(pipeline,"%u\t%u\t%u\t%u\t%u\t",frame,cpu->read_fudge,cpu->ld_absorb,cpu->ld_which_t,cpu->read_absorb_which);
    for(unsigned i=0;i<33;i++)fprintf(pipeline,"%02X",cpu->read_absorb[i]);
    fputc('\n',pipeline);fflush(pipeline);
    {const char *range=getenv("PSX_SOURCE_CPU_BOUNDARY_WINDOW");unsigned long long low,high;
     if(range&&sscanf(range,"%llu,%llu",&low,&high)==2&&cycle>=low&&cycle<=high){
      if(!mdec_states){char path[4096];const char *dir=getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
       if(!dir||snprintf(path,sizeof(path),"%s/mdec-boundary.bin",dir)>=(int)sizeof(path))abort();
       mdec_states=psx_file_create_new(path,0);if(!mdec_states)abort();}
      uint32_t n=mdec_snapshot_bytes();uint8_t *bytes=(uint8_t*)malloc(n);
      if(!n||!bytes)abort();
      mdec_snapshot_write(bytes);
      /* Private Windows observation: u32 little-endian frame,size,then exact existing wire bytes. */
      if(fwrite(&frame,4,1,mdec_states)!=1||fwrite(&n,4,1,mdec_states)!=1||fwrite(bytes,1,n,mdec_states)!=n)abort();
      free(bytes);fflush(mdec_states);
     }}
    {DMADebugState d;dma_debug_get_state(&d);
     for(unsigned i=0;i<7;i++){const DMAChannelDebugState *c=&d.channels[i];
      fprintf(dma_state,"%u\t%llu\t%08X\t%08X\t%u\t%08X\t%08X\t%08X\t%u\t%u\t%u\n",frame,(unsigned long long)cycle,d.dpcr,d.dicr,i,c->madr,c->bcr,c->chcr,c->active,c->remaining_words,c->cycles_accum);}
     fflush(dma_state);}
    {SourceGPUServiceClock g;SourceGPUCommandProjection c;
     source_gpu_runtime_copy_return(&g,&c);InputRouteRasterClock *r=&g.raster;
     fprintf(gpu_stream,"%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%u\t%u\t%u\t%u\n",frame,(unsigned long long)g.cycle,(unsigned long long)g.gpu_deadline,(unsigned long long)g.dma_deadline,(unsigned long long)g.frame_request_cycle,(unsigned long long)r->cycle,r->fraction,r->remaining,r->phase,r->scanline,r->lines,r->field,r->mode,r->start,r->end,r->blank,r->y_start,r->y_offset,r->readout_y,r->readout_field,c.budget,c.phase,c.count,c.display_mode,c.dma_direction);fflush(gpu_stream);}

    fprintf(stream,"%u\t%08X\t%llu\t%08X\t%08X\t%08X",frame,pc,(unsigned long long)cycle,cpu->cop0[12],cpu->cop0[13],cpu->cop0[14]);
    /* After the successor retired, the carried value is architecturally
     * visible even if executor write-back waits until the next instruction.
     * Project it into this passive row; never advance the execution pipeline. */
    for(unsigned i=0;i<32;i++) {
        uint32_t value=cpu->gpr[i];
        if(i && cpu->load_value_rt==i && cpu->load_value_age==1u)
            value=cpu->load_value;
        fprintf(stream,"\t%08X",value);
    }
    fputc('\n',stream);fflush(stream);
}
/* Passive, bounded diagnostic at the existing functional pre-fetch boundary.
 * It reads CPU/cache scalars only; never reads guest memory or services devices. */
static void source_cpu_boundary_probe(CPUState *cpu,uint32_t pc,uint64_t cycle) {
    /* Drain the finite always-on ring before guest polling can wrap it.
     * This observer never services a device or clears the ring. */
    static uint64_t device_next;
    if(cycle>=device_next){device_next=cycle+4096;source_device_history_probe(source_device_history_frame+1);}

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
        stream=psx_file_create_new(path,0);if(!stream)abort();
        fprintf(stream,"pc\tcycle\tcache_tag\tcache_active\tsr\tcause\tepc");
        for(unsigned i=0;i<32;i++)fprintf(stream,"\tr%u",i);
        fprintf(stream,"\tread_fudge\tld_absorb\tld_which\tread_absorb_which\tdma_read_wait\tprecise\tdirty\tinterpreter_boundary\ti_stat\ti_mask\tslice_pc\tslice_deadline\tslice_bound\tslice_cycle\tslice_takes\n");
    }
    if(++rows>500000u)abort();
    fprintf(stream,"%08X\t%llu\t%08X\t%d\t%08X\t%08X\t%08X",pc,(unsigned long long)cycle,g_psx_icache_tv[(pc&0xffcu)>>2],g_psx_icache_active,cpu->cop0[12],cpu->cop0[13],cpu->cop0[14]);
    for(unsigned i=0;i<32;i++)fprintf(stream,"\t%08X",cpu->gpr[i]);
    extern int g_precise_mode,g_dirty_interp_active,g_interp_boundary_in_progress;
    extern uint32_t i_stat,i_mask,g_slice_probe_pc,g_slice_probe_deadline,g_slice_probe_bound;
    extern uint64_t g_slice_probe_cycle,g_slice_irq_taken;
    fprintf(stream,"\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%d\t%08X\t%08X\t%08X\t%u\t%u\t%llu\t%llu\n",cpu->read_fudge,cpu->ld_absorb,cpu->ld_which_t,cpu->read_absorb_which,dma_cpu_read_penalty(),g_precise_mode,g_dirty_interp_active,g_interp_boundary_in_progress,i_stat,i_mask,g_slice_probe_pc,g_slice_probe_deadline,g_slice_probe_bound,(unsigned long long)g_slice_probe_cycle,(unsigned long long)g_slice_irq_taken);
}
#endif
