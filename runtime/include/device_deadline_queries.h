/* device_deadline_queries.h - deadline queries the scheduler inlines (PS1B-530).
 *
 * psx_cycles.c asks every device for the distance to its next event once per
 * catch-up chunk and once for each new service deadline: about 360 million
 * query sets on the Tekken 3 TAS route. The SPU-sample and source-GPU queries
 * are a few loads and compares, so their bodies live here and the scheduler
 * inlines them instead of calling into another file. The exported functions
 * (psx_spu_sample_event_cycles_to_next, source_gpu_runtime_cycles_to_event)
 * return these same bodies, so every caller gets the same value.
 *
 * The device state stays private to its file; the queries read it through the
 * read-only views declared below, which the owning file defines. */
#ifndef DEVICE_DEADLINE_QUERIES_H
#define DEVICE_DEADLINE_QUERIES_H

#include <stdint.h>
#include "psx_cycles.h"
#include "source_gpu_service_clock.h"

uint64_t psx_get_cycle_count(void);

/* interrupts.c: per-sample SPU scheduler */
typedef void (*PsxMidframeAudioPump)(void);
extern uint64_t g_spu_sample_deadline_queries;
extern uint64_t g_spu_sample_enabled_queries;
extern uint64_t g_spu_sample_mode_rejects;
extern uint64_t g_spu_sample_pump_null_rejects;
extern uint32_t g_spu_sample_last_query_phase;
extern uint32_t g_spu_sample_last_query_delta;
extern const int *const g_spu_sample_event_mode_view;            /* -1 until the env is read */
extern PsxMidframeAudioPump const *const g_midframe_audio_pump_view;
int psx_spu_sample_event_mode_init(void);

/* Distance to the next 44.1-kHz SPU sample event, or UINT32_MAX when the
 * per-sample scheduler is off or has no pump. */
static inline uint32_t psx_spu_sample_event_cycles_to_next_inline(void) {
    g_spu_sample_deadline_queries++;
    int mode = *g_spu_sample_event_mode_view;
    if (mode < 0) mode = psx_spu_sample_event_mode_init();
    if (!mode) {
        g_spu_sample_mode_rejects++;
        return UINT32_MAX;
    }
    if (!*g_midframe_audio_pump_view) {
        g_spu_sample_pump_null_rejects++;
        return UINT32_MAX;
    }
    g_spu_sample_enabled_queries++;

    /* The PS1 CPU/SPU ratio is exactly 768 CPU cycles per 44.1-kHz sample.
     * Cycle zero is the common hardware epoch; a value exactly on a sample
     * boundary names the following event, not an already-serviced event. */
    const uint32_t phase = (uint32_t)(psx_get_cycle_count() % 768u);
    const uint32_t delta = phase ? (768u - phase) : 768u;
    g_spu_sample_last_query_phase = phase;
    g_spu_sample_last_query_delta = delta;
    return delta;
}

/* source_gpu_runtime.c: the source GPU service clock */
extern const int *const g_source_gpu_runtime_enabled_view;
extern const SourceGPUServiceClock *const g_source_gpu_service_clock_view;

/* Distance to the service clock's next own or DMA tick. */
static inline uint32_t source_gpu_runtime_cycles_to_event_inline(void) {
    if (!*g_source_gpu_runtime_enabled_view) return UINT32_MAX;
    const SourceGPUServiceClock *s = g_source_gpu_service_clock_view;
    if (s->frame_pending) return 0;
    uint64_t next = source_gpu_service_next(s);
    return next > psx_cycle_count ? (uint32_t)(next - psx_cycle_count) : 1u;
}

#endif
