/* host_span_ring.c - always-on wall-time frame + span rings. See host_span_ring.h. */
#include "host_span_ring.h"

#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t qpc_now_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000ull +
           (uint64_t)((c.QuadPart % freq.QuadPart) * 1000000ull / freq.QuadPart);
}
static uint64_t this_thread_id(void) { return (uint64_t)GetCurrentThreadId(); }
#else
#include <pthread.h>
#include <time.h>
static uint64_t qpc_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t this_thread_id(void) { return (uint64_t)(uintptr_t)pthread_self(); }
#endif

static _Atomic uint64_t s_origin_us;
static _Atomic uint64_t s_emu_tid;
static _Atomic uint32_t s_cur_frame;

static HostSpan s_spans[HOST_SPAN_RING_CAP];
static _Atomic uint64_t s_span_seq;

typedef struct { uint64_t t_us; uint32_t frame; uint32_t interp; uint32_t checks; uint32_t pad; } FrameStamp;
static FrameStamp s_frames[HOST_FRAME_STAMP_CAP];
static _Atomic uint64_t s_frame_seq;

uint64_t host_clock_us(void) {
    uint64_t now = qpc_now_us();
    uint64_t origin = atomic_load_explicit(&s_origin_us, memory_order_relaxed);
    if (!origin) {
        uint64_t expect = 0;
        /* First caller fixes the origin; "now | 1" keeps "unset" (0) distinguishable. */
        uint64_t mine = now ? now : 1;
        if (atomic_compare_exchange_strong(&s_origin_us, &expect, mine))
            origin = mine;
        else
            origin = expect;
    }
    return now >= origin ? now - origin : 0;
}

uint64_t host_span_begin(void) { return host_clock_us(); }

void host_span_record(HostSpanKind kind, uint64_t t0_us, uint64_t dur_us, uint32_t arg) {
    if (dur_us < HOST_SPAN_MIN_US) return;
    uint64_t n = atomic_fetch_add_explicit(&s_span_seq, 1, memory_order_relaxed);
    HostSpan *s = &s_spans[n % HOST_SPAN_RING_CAP];
    s->t0_us = t0_us;
    s->dur_us = dur_us > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dur_us;
    s->frame = atomic_load_explicit(&s_cur_frame, memory_order_relaxed);
    s->arg = arg;
    s->kind = (uint16_t)kind;
    s->emu = this_thread_id() == atomic_load_explicit(&s_emu_tid, memory_order_relaxed);
}

void host_span_end(HostSpanKind kind, uint64_t t0_us, uint32_t arg) {
    uint64_t now = host_clock_us();
    host_span_record(kind, t0_us, now >= t0_us ? now - t0_us : 0, arg);
}

uint64_t host_span_total(void) { return atomic_load_explicit(&s_span_seq, memory_order_relaxed); }

int host_span_get(uint64_t seq, HostSpan *out) {
    uint64_t total = host_span_total();
    if (seq >= total || total - seq > HOST_SPAN_RING_CAP) return 0;
    *out = s_spans[seq % HOST_SPAN_RING_CAP];
    return 1;
}

const char *host_span_kind_name(unsigned kind) {
    static const char *const names[HSPAN_KIND_COUNT] = {
        "none", "autocapture_tick", "provider_poll", "publish_commit", "loader_rescan",
        "dll_register", "lazy_load", "capture_on_dma", "capture_before_dma", "ac_request", "ac_done_join",
        "ac_prepare_image", "ac_child", "overlay_dispatch", "mod_work", "frame_present",
    };
    return kind < HSPAN_KIND_COUNT ? names[kind] : "?";
}

void host_frame_stamp(uint32_t frame, uint32_t interp_insns, uint32_t static_checks) {
    atomic_store_explicit(&s_emu_tid, this_thread_id(), memory_order_relaxed);
    atomic_store_explicit(&s_cur_frame, frame, memory_order_relaxed);
    uint64_t n = atomic_fetch_add_explicit(&s_frame_seq, 1, memory_order_relaxed);
    FrameStamp *f = &s_frames[n % HOST_FRAME_STAMP_CAP];
    f->t_us = host_clock_us();
    f->frame = frame;
    f->interp = interp_insns;
    f->checks = static_checks;
}

uint64_t host_frame_stamp_total(void) { return atomic_load_explicit(&s_frame_seq, memory_order_relaxed); }

int host_frame_stamp_get(uint64_t seq, uint32_t *frame, uint64_t *t_us, uint32_t *interp_insns,
                         uint32_t *static_checks) {
    uint64_t total = host_frame_stamp_total();
    if (seq >= total || total - seq > HOST_FRAME_STAMP_CAP) return 0;
    const FrameStamp *f = &s_frames[seq % HOST_FRAME_STAMP_CAP];
    if (frame) *frame = f->frame;
    if (t_us) *t_us = f->t_us;
    if (interp_insns) *interp_insns = f->interp;
    if (static_checks) *static_checks = f->checks;
    return 1;
}
