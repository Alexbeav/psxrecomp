#include "source_gpu_runtime.h"
#include "psx_icache.h"
#include "psx_cycles.h"
#include "dma.h"
#include "source_cpu_boundary_probe.h"
#include "source_ram_page_probe.h"
#include <stdio.h>
#include <stdlib.h>
extern uint64_t g_psx_cycle_fast_limit;
static int enabled;
static SourceGPUServiceClock clock_state;
static SourceGPUCommandProjection command_state;
/* GPU Update consumes queued work before it advances raster state. The
 * event clock also predicts intermediate deadlines; draw parity must instead
 * follow only actual GPU service calls, plus ordinary GP1 register writes. */
static InputRouteRasterClock draw_raster;
static SourceGPUServiceClock return_clock;
static SourceGPUCommandProjection return_command;
static SourceGPUDispatchSink dispatch_sink;
void source_gpu_runtime_set_dispatch_sink(SourceGPUDispatchSink sink) {dispatch_sink=sink;}
/* Bounded leaf diagnosis. No guest reads, device service, or clock writes. */
static void command_probe(void) {
    static int initialized; static FILE *output;
    static unsigned long long low,high; static unsigned rows;
    if(!initialized) {
        initialized=1; const char *range=getenv("PSX_SOURCE_GPU_COMMAND_WINDOW");
        if(range) {
            if(sscanf(range,"%llu,%llu",&low,&high)!=2 || high<=low || high-low>1000000)abort();
            output=fopen("gpu-commands.tsv","wx");if(!output)abort();
            fputs("cycle\tbudget\tphase\tqueued\tkind\twords\n",output);
        }
    }
    uint64_t cycle=psx_get_cycle_count();
    if(output && cycle>=low && cycle<=high && command_state.dispatch.kind &&
       command_state.dispatch.kind!=SOURCE_GPU_DISPATCH_UPLOAD_WORD) {
        if(++rows>50000)abort();
        const SourceGPUCommandDispatch *d=&command_state.dispatch;
        unsigned first=d->kind==SOURCE_GPU_DISPATCH_QUAD_SECOND?
            d->count-source_gpu_polygon_stride(d->words[0]>>24):0;
        fprintf(output,"%llu\t%d\t%u\t%u\t%u\t",(unsigned long long)cycle,
                command_state.budget,command_state.phase,command_state.count,d->kind);
        for(unsigned i=first;i<d->count;i++)fprintf(output,"%s%08X",i>first?" ":"",d->words[i]);
        fputc('\n',output);
    }
    if(output && cycle>high){fclose(output);output=NULL;}
}
static void dispatch(void) {
    if(command_state.dispatch.kind && dispatch_sink)
        command_state.budget-=dispatch_sink(&command_state.dispatch);
    command_probe();
    command_state.dispatch.kind=SOURCE_GPU_DISPATCH_NONE;
}
static void fail(const char *reason) {
    fprintf(stderr,"[source-gpu-service] rejected draw mode=%04X (draw-to-display=%u)\n",command_state.draw_mode,(command_state.draw_mode>>10)&1u);
    if(command_state.count>=4 && command_state.queue[0]>>24==0x28) {
        fprintf(stderr,"[source-gpu-service] rejected first triangle vertices");
        for(unsigned i=1;i<4;i++)fprintf(stderr," %d,%d",source_gpu_command_coord(command_state.queue[i],0)+command_state.offset_x,source_gpu_command_coord(command_state.queue[i],16)+command_state.offset_y);
        fputc('\n',stderr);
    }
    /* Preserve the decisive packet on a bounded profile stop. */
    fprintf(stderr,"[source-gpu-service] queued words");
    for(unsigned i=0;i<command_state.count;i++)fprintf(stderr," %08X",command_state.queue[i]);
    fputc('\n',stderr);
    fprintf(stderr,"[source-gpu-service] %s at %llu; budget=%d phase=%u queued=%u front=%08X clip=%d,%d..%d,%d offset=%d,%d mode=%02X mask=%u\n",reason,(unsigned long long)psx_get_cycle_count(),command_state.budget,command_state.phase,command_state.count,command_state.count?command_state.queue[0]:0,command_state.clip_x0,command_state.clip_y0,command_state.clip_x1,command_state.clip_y1,command_state.offset_x,command_state.offset_y,command_state.display_mode,command_state.mask_bits);exit(2);
}
static void service(void *context,uint64_t cycle,unsigned kind) {
    (void)context;
    if(!source_gpu_command_update(&command_state,cycle))fail("unsupported command service");
    dispatch();
    if(cycle<draw_raster.cycle || cycle-draw_raster.cycle>UINT32_MAX)fail("invalid draw raster time");
    input_route_raster_advance(&draw_raster,(uint32_t)(cycle-draw_raster.cycle));
    command_state.skip_field=(draw_raster.y_start+draw_raster.readout_field)&1u;
    if(kind==SOURCE_GPU_EVENT_DMA || kind==SOURCE_GPU_EVENT_WRITE)
        dma_source_gpu_service_at(cycle);
}
static void cpu_boundary(CPUState *cpu,uint32_t pc,uint64_t cycle) {
    for(;;) {
        if(clock_state.frame_pending) {
            psx_devices_service_to_now();
            if(!source_gpu_service_cpu_boundary(&clock_state,cycle,service,0))fail("invalid CPU boundary");
            return_clock=clock_state;return_command=command_state;
            source_cpu_return_probe(cpu,pc,cycle,clock_state.frame_returns);
            source_ram_page_probe(clock_state.frame_returns,cycle);
            psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
        }
        dma_cpu_read_wait_boundary();
        source_cpu_boundary_probe(cpu,pc,cycle);
        if(!dma_cpu_source_halted())return;
        /* Original RunReal fetches before selecting its halt operation.
         * Cache fill therefore overlaps DMA, while only the base/absorb step
         * runs. No opcode effects or deferred-load writeback occur here.
         * COP2 bypasses the original interrupt/halt dispatch table; allow the
         * existing decoder to execute it, with the halt retained for its next
         * instruction. This is source compatibility, not PS1 bus arbitration. */
        uint32_t instruction=cpu->read_word(pc);
        if((instruction>>26)==0x12u)return;
        psx_icache_fetch_miss(cpu,pc);
        if(cpu->read_absorb[cpu->read_absorb_which])
            --cpu->read_absorb[cpu->read_absorb_which];
        else psx_advance_cycles(1u);
        psx_cyc_batch_flush();
        psx_devices_service_to_now();
        cycle=psx_get_cycle_count();
    }
}

void source_gpu_runtime_init(void) {
    if(enabled || psx_get_cycle_count()!=0 || g_psx_cpu_step_boundary_callback)fail("cold initialization/CPU owner required");
    source_gpu_service_cold(&clock_state);source_gpu_command_cold(&command_state);
    input_route_raster_reset(&draw_raster);command_state.field_valid=1;
    enabled=1;g_psx_cpu_step_boundary_callback=cpu_boundary;
    psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
}
int source_gpu_runtime_active(void) {return enabled;}
int source_gpu_runtime_ready(void) {return enabled?source_gpu_command_ready(&command_state):-2;}
uint32_t source_gpu_runtime_status_bits(void) {
    uint32_t bits=(command_state.dma_direction&2u)?1u<<25:0;
    if(!command_state.phase && !command_state.count && command_state.budget>=0)bits|=1u<<26;
    if(source_gpu_command_ready(&command_state)>0)bits|=1u<<28;
    if(command_state.phase==8)bits|=1u<<27;
    return bits;
}
void source_gpu_runtime_advance(void) {
    if(enabled && !source_gpu_service_to(&clock_state,psx_cycle_count,service,0))fail("reversed device time");
}
uint32_t source_gpu_runtime_cycles_to_event(void) {
    if(!enabled)return UINT32_MAX;
    uint64_t next=source_gpu_service_next(&clock_state);
    return next>psx_cycle_count?(uint32_t)(next-psx_cycle_count):1u;
}
void source_gpu_runtime_dma_write(void) {
    if(enabled && !source_gpu_service_dma_write(&clock_state,psx_cycle_count,service,0))fail("invalid DMA write time");
}
void source_gpu_runtime_gp0(uint32_t word) {
    if(!enabled)return;
    unsigned previous=command_state.first_triangles;
    if(!source_gpu_command_write(&command_state,word))fail("unsupported GP0 word");
    dispatch();
    if(!previous && command_state.first_triangles)
        fprintf(stderr,"[source-gpu-service] first quad at %llu; budget=%d phase=%u queued=%u clip=%d,%d..%d,%d offset=%d,%d mode=%02X mask=%u\n",(unsigned long long)psx_get_cycle_count(),command_state.budget,command_state.phase,command_state.count,command_state.clip_x0,command_state.clip_y0,command_state.clip_x1,command_state.clip_y1,command_state.offset_x,command_state.offset_y,command_state.display_mode,command_state.mask_bits);
}
void source_gpu_runtime_read(void) {
    if(enabled)source_gpu_command_read(&command_state);
}
void source_gpu_runtime_gp1(uint32_t word) {
    if(!enabled)return;
    source_gpu_runtime_advance();
    if(!source_gpu_command_gp1(&command_state,word) || !input_route_raster_gp1(&clock_state.raster,word) || !input_route_raster_gp1(&draw_raster,word))fail("unsupported GP1 control");
    command_state.skip_field=(draw_raster.y_start+draw_raster.readout_field)&1u;
    psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
}
void source_gpu_runtime_copy(SourceGPUServiceClock *clock,SourceGPUCommandProjection *command) {
    if(clock)*clock=clock_state;
    if(command)*command=command_state;
}
void source_gpu_runtime_copy_return(SourceGPUServiceClock *clock,SourceGPUCommandProjection *command) {
    if(clock)*clock=return_clock;
    if(command)*command=return_command;
}
