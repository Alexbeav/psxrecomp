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

#include <stddef.h>
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

/* One guest store made inside a pass (memory.c render_pass_store): RAM and
 * scratchpad are written directly, bypassing every live-timeline observer;
 * MMIO reaches `mmio_write` only when render_pass_mmio_class allows it; KSEG2
 * (cache control), mod memory, expansion and ROM are dropped. Returns -1 when
 * the store was made (or absorbed: an isolated-cache store changes nothing a
 * pass restores), else the RENDER_PASS_DROP_* class it was dropped under,
 * which the caller counts. */
typedef struct RenderPassStoreTarget {
    uint8_t *ram;              /* main RAM, ram_size bytes (mirrored) */
    uint32_t ram_size;
    uint8_t *scratchpad;
    uint32_t scratchpad_size;
    int      isolate_cache;    /* COP0 SR IsC is set */
    void   (*mmio_write)(uint32_t phys, uint32_t val, uint32_t width);
} RenderPassStoreTarget;
int render_pass_store_to(const RenderPassStoreTarget *t, uint32_t addr,
                         uint32_t val, uint32_t width);

/* VRAM journal for a pass's writes outside its display rect
 * (gpu_gl_renderer.c pass_refuse_write): the CPU-side policy and the CPU VRAM
 * rows. The renderer keeps the matching GPU copies per entry. */
#define RENDER_PASS_JOURNAL_MAX 16
enum {
    RENDER_PASS_VRAM_ALLOW = 0,    /* inside the rect, empty, or already journaled */
    RENDER_PASS_VRAM_JOURNAL = 1,  /* outside: back up, then allow */
    RENDER_PASS_VRAM_REFUSE = 2    /* outside and cannot be journaled */
};
typedef struct RenderPassJournal {
    int      n;
    int      x[RENDER_PASS_JOURNAL_MAX], y[RENDER_PASS_JOURNAL_MAX];
    int      w[RENDER_PASS_JOURNAL_MAX], h[RENDER_PASS_JOURNAL_MAX];
    uint16_t *rows[RENDER_PASS_JOURNAL_MAX];
    size_t   cap[RENDER_PASS_JOURNAL_MAX];
} RenderPassJournal;
/* Clip the write (*x, *y, *w, *h) to vram_w x vram_h and decide it against
 * the pass rect (px, py, pw, ph) and the journal. can_journal = 0 refuses
 * every outside write (e.g. native-wide margins are not journaled). */
int  render_pass_vram_policy(const RenderPassJournal *j, int px, int py,
                             int pw, int ph, int vram_w, int vram_h,
                             int can_journal, int *x, int *y, int *w, int *h);
/* Record the CPU rows of rect (x, y, w, h) of vram (vram_w halfwords per
 * row) as the next entry. Returns its index, or -1 when full / out of
 * memory (nothing recorded). */
int  render_pass_journal_add(RenderPassJournal *j, const uint16_t *vram,
                             int vram_w, int x, int y, int w, int h);
/* Put every entry's rows back, newest first, and empty the journal. */
void render_pass_journal_rollback(RenderPassJournal *j, uint16_t *vram,
                                  int vram_w);
void render_pass_journal_free(RenderPassJournal *j);

#ifdef __cplusplus
}
#endif

#endif
