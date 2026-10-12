/* host_overlay.c -- see host_overlay.h. */
#include "host_overlay.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#endif

/* ---- P1: host pause ------------------------------------------------------ */
#define PSX_HOST_PAUSE_MAX 8
static int s_pause_depth = 0;
static const char *s_pause_reason[PSX_HOST_PAUSE_MAX];
static PsxHostPauseRefuseFn s_pause_refuse = NULL;

int psx_host_pause_push(const char *reason) {
    if (s_pause_refuse && s_pause_refuse()) return 0;
    if (s_pause_depth >= PSX_HOST_PAUSE_MAX) return 0;
    s_pause_reason[s_pause_depth++] = reason ? reason : "host";
    return 1;
}

void psx_host_pause_pop(void) {
    if (s_pause_depth > 0) s_pause_reason[--s_pause_depth] = NULL;
}

int psx_host_pause_depth(void) { return s_pause_depth; }

const char *psx_host_pause_reason(void) {
    return s_pause_depth > 0 ? s_pause_reason[s_pause_depth - 1] : NULL;
}

void psx_host_pause_set_refuse_probe(PsxHostPauseRefuseFn fn) { s_pause_refuse = fn; }

/* ---- P2: overlay draw callback -------------------------------------------- */
/* Registered on the main thread, read on the render / present thread. The
 * function and its context are published together as one immutable record
 * behind an atomic pointer, so a reader never pairs a live function with a
 * stale context.
 *
 * Lifetime: every call runs inside an in-flight count. Replacing or removing
 * the callback swaps the record and then waits until no call that could have
 * read the old record is still running, so when psx_host_overlay_set_draw_cb
 * returns, the previous fn will not run again and its ctx (and the GL objects
 * it draws with) may be freed by the caller. The old record is freed there
 * too. A change made from inside the callback itself does not wait (it would
 * wait for itself); that caller's own frame is the only one still using the
 * old ctx and it returns before the next frame reads the new record. */
typedef struct {
    PsxHostOverlayDrawFn fn;
    void *ctx;
} PsxHostOverlayDraw;

static _Atomic(PsxHostOverlayDraw *) s_draw = NULL;
static atomic_int s_draw_inflight = 0;
#if defined(_MSC_VER)
static __declspec(thread) int s_in_draw = 0;
#else
static _Thread_local int s_in_draw = 0;
#endif

static void draw_yield(void) {
#if defined(_WIN32)
    Sleep(0);
#else
    sched_yield();
#endif
}

void psx_host_overlay_set_draw_cb(PsxHostOverlayDrawFn fn, void *ctx) {
    PsxHostOverlayDraw *rec = NULL, *old;
    if (fn) {
        rec = (PsxHostOverlayDraw *)malloc(sizeof(*rec));
        if (!rec) return;
        rec->fn = fn;
        rec->ctx = ctx;
    }
    old = atomic_exchange(&s_draw, rec); /* seq_cst: ordered with the readers' count */
    if (s_in_draw) return; /* from inside the callback: see above; old stays (rare) */
    while (atomic_load(&s_draw_inflight) > 0) draw_yield();
    free(old);
}

int psx_host_overlay_has_draw(void) {
    return atomic_load_explicit(&s_draw, memory_order_acquire) != NULL;
}

void psx_host_overlay_draw(int width, int height) {
    PsxHostOverlayDraw *rec;
    if (width <= 0 || height <= 0) return;
    atomic_fetch_add(&s_draw_inflight, 1); /* seq_cst, before reading the record */
    rec = atomic_load(&s_draw);
    if (rec && rec->fn) {
        s_in_draw = 1;
        rec->fn(width, height, rec->ctx);
        s_in_draw = 0;
    }
    atomic_fetch_sub(&s_draw_inflight, 1);
}

/* ---- P3: input sink -------------------------------------------------------- */
static PsxHostInputSinkFn s_sink_fn = NULL;
static void *s_sink_ctx = NULL;
static volatile int s_ui_capture = 0;

void psx_host_set_input_sink(PsxHostInputSinkFn fn, void *ctx) {
    s_sink_fn = fn;
    s_sink_ctx = ctx;
}

int psx_host_input_sink_dispatch(const void *event) {
    return (s_sink_fn && event) ? (s_sink_fn(event, s_sink_ctx) != 0) : 0;
}

void psx_host_set_ui_capture(int on) { s_ui_capture = on ? 1 : 0; }
int psx_host_ui_capture_active(void) { return s_ui_capture; }

/* ---- P4: live video settings ------------------------------------------------ */
unsigned psx_host_video_live_changes(const PsxHostVideoSettings *a,
                                     const PsxHostVideoSettings *b) {
    unsigned m = 0;
    if (!a || !b) return 0;
    if (a->fullscreen != b->fullscreen) m |= PSX_VIDEO_LIVE_FULLSCREEN;
    if (a->texture_filter != b->texture_filter) m |= PSX_VIDEO_LIVE_TEXTURE_FILTER;
    if (a->fmv_filter != b->fmv_filter) m |= PSX_VIDEO_LIVE_FMV_FILTER;
    if (a->scanlines != b->scanlines || a->scanline_strength_pct != b->scanline_strength_pct)
        m |= PSX_VIDEO_LIVE_SCANLINES;
    if (a->present_linear != b->present_linear) m |= PSX_VIDEO_LIVE_PRESENT_LINEAR;
    if (a->dynamic_resolution != b->dynamic_resolution) m |= PSX_VIDEO_LIVE_DYNRES;
    if (a->frame_generation != b->frame_generation) m |= PSX_VIDEO_LIVE_FRAME_GEN;
    return m;
}

unsigned psx_host_video_restart_bits(const PsxHostVideoSettings *boot,
                                     const PsxHostVideoSettings *next) {
    unsigned m = 0;
    if (!boot || !next) return 0;
    if (boot->renderer != next->renderer) m |= PSX_VIDEO_RESTART_RENDERER;
    if (boot->internal_resolution != next->internal_resolution ||
        boot->supersampling != next->supersampling)
        m |= PSX_VIDEO_RESTART_RESOLUTION;
    if (boot->render_thread != next->render_thread ||
        boot->present_thread != next->present_thread)
        m |= PSX_VIDEO_RESTART_THREADS;
    return m;
}

int psx_texture_filter_from_ui(int value) {
    return (value >= PSX_TEXTURE_FILTER_NEAREST && value <= PSX_TEXTURE_FILTER_XBR)
               ? value : PSX_TEXTURE_FILTER_NEAREST;
}
