/* gl_scale_limits.h — how large an internal-resolution scale the OpenGL
 * backend may allocate.
 *
 * Pure arithmetic, no GL or SDL, so a test can pin it. The GL backend keeps the
 * authoritative VRAM as one 1024*S x 512*S colour texture plus a same-size
 * depth-stencil renderbuffer (gpu_gl_renderer.c). Two things bound S:
 *
 *   - the driver's size limits: GL_MAX_TEXTURE_SIZE, GL_MAX_RENDERBUFFER_SIZE
 *     and GL_MAX_VIEWPORT_DIMS (the caller passes their minimum). Apple's GL
 *     on Metal reports 16384, so the full-VRAM surface tops out at S = 16;
 *   - a memory budget. On unified-memory machines this is system RAM.
 *
 * A request that does not fit is clamped here, never turned into a failed
 * context: the old behaviour (allocation failure -> the whole GL backend fell
 * back to the software renderer) lost far more than the resolution. */
#pragma once

#include <stdint.h>

#define PSX_GL_VRAM_W 1024
#define PSX_GL_VRAM_H 512

/* RGBA8 colour + D24S8 depth-stencil per internal pixel. The copy scratch is
 * sized lazily to the largest copy, so it is not part of the fixed cost. */
#define PSX_GL_HR_BYTES_PER_PX 8u

/* Default memory budget for the full-VRAM surface: 2 GiB (S = 22 by memory;
 * the texture limit binds first on 16384-limit GPUs). */
#define PSX_GL_DEFAULT_BUDGET_MB 2048u

/* Why a scale was reduced. */
enum {
    PSX_GL_SCALE_OK       = 0,
    PSX_GL_SCALE_TEXTURE  = 1,  /* 1024*S exceeds the driver size limit */
    PSX_GL_SCALE_BUDGET   = 2,  /* surface exceeds the memory budget */
    PSX_GL_SCALE_CEILING  = 4   /* above the compile-time ceiling */
};

/* Bytes of the full-VRAM hr surface (colour + depth-stencil) at scale s. */
static inline uint64_t psx_gl_full_vram_bytes(int s) {
    if (s < 1) s = 1;
    return (uint64_t)PSX_GL_VRAM_W * (uint64_t)s *
           (uint64_t)PSX_GL_VRAM_H * (uint64_t)s * PSX_GL_HR_BYTES_PER_PX;
}

/* 1 if a full-VRAM surface at scale s fits max_dim and budget_bytes.
 * max_dim <= 0 means "unknown" (no size check). budget_bytes == 0 means no
 * budget. */
static inline int psx_gl_full_vram_fits(int s, int max_dim, uint64_t budget_bytes) {
    if (s < 1) return 0;
    if (max_dim > 0 && (int64_t)PSX_GL_VRAM_W * s > (int64_t)max_dim) return 0;
    if (max_dim > 0 && (int64_t)PSX_GL_VRAM_H * s > (int64_t)max_dim) return 0;
    if (budget_bytes && psx_gl_full_vram_bytes(s) > budget_bytes) return 0;
    return 1;
}

/* Largest scale <= req whose full-VRAM surface fits. Never below 1. When
 * reason is non-NULL it receives a PSX_GL_SCALE_* mask of what bound it. */
static inline int psx_gl_clamp_full_vram_scale(int req, int ceiling, int max_dim,
                                               uint64_t budget_bytes, int *reason) {
    int why = PSX_GL_SCALE_OK;
    int s = req < 1 ? 1 : req;
    if (ceiling > 0 && s > ceiling) { s = ceiling; why |= PSX_GL_SCALE_CEILING; }
    while (s > 1) {
        int tex_ok = !(max_dim > 0 &&
                       ((int64_t)PSX_GL_VRAM_W * s > (int64_t)max_dim ||
                        (int64_t)PSX_GL_VRAM_H * s > (int64_t)max_dim));
        int mem_ok = !(budget_bytes && psx_gl_full_vram_bytes(s) > budget_bytes);
        if (tex_ok && mem_ok) break;
        if (!tex_ok) why |= PSX_GL_SCALE_TEXTURE;
        if (!mem_ok) why |= PSX_GL_SCALE_BUDGET;
        s--;
    }
    if (reason) *reason = why;
    return s;
}

/* Widest native-wide surface (in native px) that fits max_dim at scale s.
 * The GL wide surfaces are wide_w*S x 512*S. Returns 0 when nothing fits. */
static inline int psx_gl_max_wide_width(int s, int max_dim) {
    if (s < 1) s = 1;
    if (max_dim <= 0) return 1 << 30;
    if ((int64_t)PSX_GL_VRAM_H * s > (int64_t)max_dim) return 0;
    return max_dim / s;
}

/* Native-wide surface width (native px) for a display disp_w px wide at aspect
 * num:den: the display plus two margins of disp_w*(3*num - 4*den)/(8*den),
 * rounded exactly as gpu.c's ws_nw_configured_offset rounds them (16:9, 21:9
 * and 32:9 at 320 wide give 426, 560 and 854). */
static inline int psx_gl_native_wide_width(int disp_w, int num, int den) {
    if (disp_w <= 0 || num <= 0 || den <= 0 ||
        (int64_t)num * 3 <= (int64_t)den * 4)
        return disp_w;
    int64_t numr = (int64_t)3 * num - (int64_t)4 * den;
    return disp_w + 2 * (int)(((int64_t)disp_w * numr + (int64_t)4 * den) /
                              ((int64_t)8 * den));
}

/* Narrow num:den, if needed, to the widest aspect whose native-wide surface
 * is at most max_w native px (psx_gl_max_wide_width) at display width disp_w;
 * 4:3 when not even one more column fits. 4*(max_w-1) : 3*disp_w rounds to at
 * most max_w columns. Returns 1 when it changed num:den. */
static inline int psx_gl_fit_wide_aspect(int disp_w, int max_w, int *num, int *den) {
    if (disp_w <= 0 || max_w <= 0 || !num || !den ||
        psx_gl_native_wide_width(disp_w, *num, *den) <= max_w)
        return 0;
    int64_t n = 4, d = 3;
    if (max_w - 1 > disp_w) {
        n = 4 * (int64_t)(max_w - 1);
        d = 3 * (int64_t)disp_w;
        int64_t a = n, b = d;
        while (b) { int64_t t = a % b; a = b; b = t; }
        n /= a; d /= a;
    }
    *num = (int)n;
    *den = (int)d;
    return 1;
}

/* Parse a budget override in MiB (PSX_GL_VRAM_BUDGET_MB). NULL/empty/invalid
 * returns the default. "0" disables the budget. */
static inline uint64_t psx_gl_budget_bytes_from_env(const char *mb) {
    uint64_t v = 0;
    const char *p = mb;
    if (!p || !*p) return (uint64_t)PSX_GL_DEFAULT_BUDGET_MB << 20;
    while (*p >= '0' && *p <= '9') { v = v * 10u + (uint64_t)(*p - '0'); p++; if (v > (1u << 24)) break; }
    if (*p) return (uint64_t)PSX_GL_DEFAULT_BUDGET_MB << 20;
    return v << 20;
}
