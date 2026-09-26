#ifndef PSX_RENDER_PASS_PLAN_H
#define PSX_RENDER_PASS_PLAN_H

/* Pure planning and selection math for host-timed render passes
 * (render_pass.c, gpu_gl_renderer.c). No runtime state; unit-tested by
 * runtime/tests/test_render_pass_plan.c.
 *
 * Vocabulary. A game frame F is first presented at host tick `frame_start`
 * and stays on screen for `frame_length` host ticks (its flip period in guest
 * VBlanks times the presenter's source period). Phase p in [0, 1) is the
 * position inside that window. Phase 0 is the game's own image of F; a pass
 * rendered at phase a shows the game state a of the way from F to the next
 * frame. Phases are carried as Q16 (65536 = 1). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RENDER_PASS_MAX_PHASES 16u

typedef struct RenderPassPlanInput {
    double next_deadline;   /* the presenter's next output deadline (ticks) */
    double target_period;   /* ticks per output frame; <= 0 = unknown */
    double frame_start;     /* predicted first-present tick of the frame */
    double frame_length;    /* ticks the frame stays on screen */
    double pass_cost;       /* smoothed host ticks per pass; 0 = unknown */
    double budget;          /* host ticks the passes may use; < 0 = unlimited */
    uint32_t max;           /* caller's array capacity */
} RenderPassPlanInput;

/* Fill alpha_q16[] with the ascending phases the presenter will show during
 * the frame (the output deadlines that fall strictly inside it, excluding
 * ones within 1/64 of phase 0, which the game's own image already covers).
 * When the budget cannot pay for all of them, an evenly spread subset is
 * returned; *wanted (optional) receives the unshed count. Returns the count
 * (0 when nothing is wanted or affordable). */
uint32_t render_pass_plan_phases(const RenderPassPlanInput *in,
                                 uint32_t *alpha_q16, uint32_t *wanted);

/* Pick what to show at phase p (0..1+) from a frame's rendered items.
 * phases[0..n) are ascending Q16 phases, phases[0] == 0 being the game's
 * image. Sets *lo and *hi (item indices) and *t (blend weight of hi, 0..1):
 * inside [phases[i], phases[i+1]] the two neighbours are blended; past the
 * last item it is held (nothing newer exists yet). Returns 0 when n == 0. */
int render_pass_select(const uint32_t *phases, uint32_t n, double p,
                       uint32_t *lo, uint32_t *hi, float *t);

/* Exponential moving average used for the per-pass host cost. */
double render_pass_ema(double current, double sample);

/* Next frame's pass budget from the last frame: the host time the presenter
 * spent idle-waiting plus the time passes used, scaled by `share` (0..1) and
 * clamped to [0, frame_length]. No history (both zero) -> share of the frame. */
double render_pass_budget(double idle_ticks, double pass_ticks,
                          double frame_length, double share);

/* Store policy inside a pass (memory.c): -1 = the MMIO store may reach its
 * device (GP0; GP1 DMA mode 0x04 / info 0x10; GPU and OTC DMA channels;
 * DPCR/DICR; I_STAT/I_MASK -- all restored after the pass), otherwise the
 * RENDER_PASS_DROP_* class it is dropped and counted under (render_pass.h). */
int render_pass_mmio_class(uint32_t phys, uint32_t val, uint32_t width);

#ifdef __cplusplus
}
#endif

#endif
