#ifndef PSX_SOURCE_GPU_SERVICE_CLOCK_H
#define PSX_SOURCE_GPU_SERVICE_CLOCK_H
#include "input_route_raster_clock.h"

/* Standalone source event clock, not yet connected to runtime scheduling.
 * Original DMA_Update services the GPU on its fixed 128-cycle cadence and
 * before DMA register writes. GPU's own event reschedules from the earlier
 * of 128 clocks and the next raster phase boundary. A DMA caller does not
 * replace that own-event deadline. Only NTSC cold state is represented here.
 * Placement of frame-end ForceEventUpdates, mode writes and reset/restore
 * remain separate integration gates. Callback duplicates are intentional:
 * the command projection must make zero-elapsed Update a no-op.
 */
typedef struct SourceGPUServiceClock {
    InputRouteRasterClock raster;
    uint64_t cycle, gpu_deadline, dma_deadline;
    uint64_t frame_request_cycle;
    unsigned zero_reached,frame_pending,frame_returns;
} SourceGPUServiceClock;
typedef void (*SourceGPUServiceEvent)(void *,uint64_t,unsigned);
enum { SOURCE_GPU_EVENT_OWN=1, SOURCE_GPU_EVENT_DMA=2, SOURCE_GPU_EVENT_WRITE=3,
       SOURCE_GPU_EVENT_FRAME_END=4 };
static inline void source_gpu_service_cold(SourceGPUServiceClock *s) {
    memset(s,0,sizeof(*s)); input_route_raster_reset(&s->raster);
    s->gpu_deadline=s->dma_deadline=128;
}
static inline uint64_t source_gpu_service_next(const SourceGPUServiceClock *s) {
    return s->gpu_deadline<s->dma_deadline ? s->gpu_deadline : s->dma_deadline;
}
static inline uint32_t source_gpu_service_until_phase(const SourceGPUServiceClock *s) {
    uint64_t clocks=(uint64_t)s->raster.remaining*65536u-s->raster.fraction;
    return (uint32_t)((clocks+103895u)/103896u);
}
/* Original frontend requests its return only after passing scanline zero.
 * Visible/final-line fallbacks and the >=232 VBlank rule are distinct from
 * the hardware IRQ edge. The CPU boundary consumes the request later. */
static inline void source_gpu_service_raster(SourceGPUServiceClock *s,uint32_t elapsed) {
    uint32_t old_line=s->raster.scanline,old_blank=s->raster.blank;
    input_route_raster_advance(&s->raster,elapsed);
    if(s->raster.scanline==old_line) return;
    uint32_t line=s->raster.scanline;
    if(!line)s->zero_reached=1;
    if(s->zero_reached && (line==256 || line==s->raster.lines-1 ||
       (!old_blank && s->raster.blank && line>=232))) {
        if(!s->frame_pending)s->frame_request_cycle=s->raster.cycle;
        s->frame_pending=1;
    }
}
static inline int source_gpu_service_to(SourceGPUServiceClock *s,uint64_t now,
        SourceGPUServiceEvent event,void *context) {
    if(now<s->cycle) return 0;
    uint64_t next;
    while((next=source_gpu_service_next(s))<=now) {
        uint64_t elapsed=next-s->cycle;
        if(elapsed>UINT32_MAX) return 0;
        source_gpu_service_raster(s,(uint32_t)elapsed); s->cycle=next;
        if(next==s->gpu_deadline) {
            event(context,next,SOURCE_GPU_EVENT_OWN);
            uint32_t distance=source_gpu_service_until_phase(s);
            s->gpu_deadline=next+(distance<128 ? distance : 128);
        }
        if(next==s->dma_deadline) {
            event(context,next,SOURCE_GPU_EVENT_DMA); s->dma_deadline+=128;
        }
    }
    uint64_t elapsed=now-s->cycle;
    if(elapsed>UINT32_MAX) return 0;
    source_gpu_service_raster(s,(uint32_t)elapsed); s->cycle=now;
    return 1;
}
static inline int source_gpu_service_dma_write(SourceGPUServiceClock *s,uint64_t now,
        SourceGPUServiceEvent event,void *context) {
    if(!source_gpu_service_to(s,now,event,context)) return 0;
    event(context,now,SOURCE_GPU_EVENT_WRITE); return 1;
}
/* Original frame-end ForceEventUpdates runs GPU then DMA at the returned CPU
 * timestamp, and replaces the GPU event deadline even for a zero-time update.
 * This absolute-clock representation needs no timestamp rebase. The caller
 * must supply an independently qualified source-equivalent return boundary. */
static inline int source_gpu_service_frame_end(SourceGPUServiceClock *s,uint64_t now,
        SourceGPUServiceEvent event,void *context) {
    if(!source_gpu_service_to(s,now,event,context)) return 0;
    event(context,now,SOURCE_GPU_EVENT_FRAME_END);
    uint32_t distance=source_gpu_service_until_phase(s);
    s->gpu_deadline=now+(distance<128 ? distance : 128);
    event(context,now,SOURCE_GPU_EVENT_DMA);
    s->frame_pending=0;s->zero_reached=0;s->frame_returns++;
    return 1;
}
static inline int source_gpu_service_cpu_boundary(SourceGPUServiceClock *s,uint64_t now,
        SourceGPUServiceEvent event,void *context) {
    if(!source_gpu_service_to(s,now,event,context)) return 0;
    return !s->frame_pending || source_gpu_service_frame_end(s,now,event,context);
}
#endif
