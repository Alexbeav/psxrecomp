#ifndef PSX_FRAME_INTERPOLATION_H
#define PSX_FRAME_INTERPOLATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameInterpolationSchedule {
    double source_deadline;
    double next_present_deadline;
    double frame_start;
    double frame_end;
    double target_period;       /* 0 = unlimited (present back-to-back) */
    int    presents;            /* presents issued in the current interval */
    int    starved;             /* consecutive intervals with no present   */
    double present_cost;        /* caller-measured ticks per present (EMA);
                                 * unlimited mode stops starting presents
                                 * that would overrun the guest interval */
} FrameInterpolationSchedule;

/* Fold one measured present duration (host ticks) into the cost estimate. */
void frame_interpolation_schedule_note_cost(FrameInterpolationSchedule *schedule,
                                            uint64_t ticks);

/* Plan one stock guest-frame interval. Presentation deadlines remain anchored
 * across calls so guest work consumes part of the interval instead of slowing
 * the simulation. Returns zero when the rates or host clock are invalid.
 *
 * target_hz is the presentation-rate limit and is independent of source_hz:
 * above the source rate an interval holds several presents, below it some
 * intervals hold none (a 30 fps cap on a 50 Hz guest). target_hz <= 0 means
 * unlimited: present back-to-back until the interval ends, at least once. */
int frame_interpolation_schedule_begin(FrameInterpolationSchedule *schedule,
                                       uint64_t now, uint64_t frequency,
                                       double source_hz, double target_hz);

/* Return each presentation deadline in the current source interval. Alpha is
 * the temporal blend position from the previous completed frame to the current
 * one. Deadlines missed by more than one output period are coalesced. */
int frame_interpolation_schedule_next(FrameInterpolationSchedule *schedule,
                                      uint64_t now, uint64_t *deadline,
                                      float *alpha);

/* Map a blend position inside one guest VBlank interval onto the span of a
 * game frame that lasted `frame_vblanks` VBlanks (frame_cadence.h), given how
 * many VBlanks have passed since that frame was captured. A title that flips
 * every second VBlank is blended across both VBlanks instead of blending in
 * one and holding in the next. Clamped to [0, 1]: a late frame holds. */
float frame_interpolation_span_alpha(float interval_alpha,
                                     uint32_t vblanks_since_capture,
                                     uint32_t frame_vblanks);

uint64_t frame_interpolation_schedule_end(
    const FrameInterpolationSchedule *schedule);
void frame_interpolation_schedule_reset(FrameInterpolationSchedule *schedule);

#ifdef __cplusplus
}
#endif

#endif
