#ifndef PSX_SOURCE_GPU_SERVICE_CLOCK_H
#define PSX_SOURCE_GPU_SERVICE_CLOCK_H
#include "input_route_raster_clock.h"

typedef struct SourceGPUServiceClock {
    InputRouteRasterClock raster;
    uint64_t cycle, gpu_deadline, dma_deadline, frame_request_cycle;
    unsigned zero_reached, frame_pending, frame_returns;
} SourceGPUServiceClock;
typedef void (*SourceGPUServiceEvent)(void *, uint64_t, unsigned);
enum { SOURCE_GPU_EVENT_OWN = 1, SOURCE_GPU_EVENT_DMA = 2,
       SOURCE_GPU_EVENT_WRITE = 3, SOURCE_GPU_EVENT_FRAME_END = 4 };

/* Independent measured scheduling model; see service_clock_provenance.json. */
static inline void source_gpu_service_cold(SourceGPUServiceClock *s)
{
    *s = (SourceGPUServiceClock){0};
    input_route_raster_reset(&s->raster);
    s->gpu_deadline = s->dma_deadline = 128;
}

static inline uint64_t source_gpu_service_next(const SourceGPUServiceClock *s)
{
    return s->gpu_deadline < s->dma_deadline ? s->gpu_deadline : s->dma_deadline;
}

static inline uint32_t source_gpu_service_until_phase(const SourceGPUServiceClock *s)
{
    return (uint32_t)(((uint64_t)s->raster.remaining * 65536 - s->raster.fraction + 103895) / 103896);
}

static inline void source_gpu_service_raster(SourceGPUServiceClock *s, uint32_t elapsed)
{
    unsigned old_line = s->raster.scanline, old_blank = s->raster.blank;
    input_route_raster_advance(&s->raster, elapsed);
    int changed_line = old_line != s->raster.scanline;
    if (changed_line && !s->raster.scanline) s->zero_reached = 1;
    int frame_edge = (!old_blank && s->raster.blank && s->raster.scanline >= 232)
                     || (changed_line && (s->raster.scanline == 256
                                          || s->raster.scanline + 1 == s->raster.lines));
    if (s->zero_reached && !s->frame_pending && frame_edge) {
        s->frame_pending = 1;
        s->frame_request_cycle = s->raster.cycle;
    }
}

static inline uint64_t source_gpu_service_own_deadline(const SourceGPUServiceClock *s)
{
    uint32_t delta = source_gpu_service_until_phase(s);
    return s->cycle + (delta < 128 ? delta : 128);
}

static inline int source_gpu_service_to(SourceGPUServiceClock *s, uint64_t now,
    SourceGPUServiceEvent event, void *context)
{
    if (now < s->cycle) return 0;
    uint64_t next;
    while ((next = source_gpu_service_next(s)) <= now) {
        source_gpu_service_raster(s, (uint32_t)(next - s->cycle));
        s->cycle = next;
        if (s->gpu_deadline <= next) {
            if (event) event(context, next, SOURCE_GPU_EVENT_OWN);
            s->gpu_deadline = source_gpu_service_own_deadline(s);
        }
        if (s->dma_deadline <= next) {
            if (event) event(context, next, SOURCE_GPU_EVENT_DMA);
            s->dma_deadline = next + 128;
        }
    }
    source_gpu_service_raster(s, (uint32_t)(now - s->cycle));
    s->cycle = now;
    return 1;
}

static inline int source_gpu_service_dma_write(SourceGPUServiceClock *s, uint64_t now,
    SourceGPUServiceEvent event, void *context)
{
    if (!source_gpu_service_to(s, now, event, context)) return 0;
    if (event) event(context, now, SOURCE_GPU_EVENT_WRITE);
    return 1;
}

static inline int source_gpu_service_frame_end(SourceGPUServiceClock *s, uint64_t now,
    SourceGPUServiceEvent event, void *context)
{
    if (!source_gpu_service_to(s, now, event, context)) return 0;
    if (event) event(context, now, SOURCE_GPU_EVENT_FRAME_END);
    s->gpu_deadline = source_gpu_service_own_deadline(s);
    if (event) event(context, now, SOURCE_GPU_EVENT_DMA);
    s->zero_reached = s->frame_pending = 0;
    ++s->frame_returns;
    return 1;
}

static inline int source_gpu_service_cpu_boundary(SourceGPUServiceClock *s, uint64_t now,
    SourceGPUServiceEvent event, void *context)
{
    if (!source_gpu_service_to(s, now, event, context)) return 0;
    return s->frame_pending ? source_gpu_service_frame_end(s, now, event, context) : 1;
}
#endif
