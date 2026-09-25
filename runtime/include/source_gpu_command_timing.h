/* Source GPU command timing table. Approved by Alex 2026-09-25 (PS1B-166 G2).
 *
 * Budget unit: one 33.8688 MHz CPU clock. Costs are summed in quarter clocks
 * and rounded up to whole clocks once per command.
 *
 * [DOC] No$PSX "GPU Rendering Timings" (problemkaputt.de/psx-spx.htm fetched
 *       2026-09-25, SHA-256 7879668170a8...; section extract a3b2131f3774...).
 *       New GPU values are used throughout; Old GPU values are ignored.
 *       FIFO depth and prefetch: No$PSX "GPU FIFO" (section extract 5767a2b3a5c8...).
 * [TEST] test-derived; origin = pre-T172 test goldens; hardware measurement
 *       owed. Used only for credit bookkeeping and model-scope rules, which
 *       the documentation does not cover.
 */
#ifndef PSX_SOURCE_GPU_COMMAND_TIMING_H
#define PSX_SOURCE_GPU_COMMAND_TIMING_H

#define SOURCE_GPU_T_CLOCKS(quarters) ((int)(((quarters) + 3) / 4))

/* ---- Credit ---------------------------------------------------------------- */

/* [DOC] "Oscillators": rendering is bound to the 33 MHz CPU clock, so one
 * elapsed CPU cycle is one clock of credit. */
#define SOURCE_GPU_T_CREDIT_PER_CYCLE 1
/* [TEST] command_projection:49 — the most credit an idle GPU banks. */
#define SOURCE_GPU_T_CREDIT_LIMIT 256
#define SOURCE_GPU_T_CREDIT(s, elapsed) source_gpu_t_credit((s)->budget, (elapsed))
static inline int32_t source_gpu_t_credit(int32_t budget, uint64_t elapsed)
{
    int64_t credit = (int64_t)budget + (int64_t)elapsed * SOURCE_GPU_T_CREDIT_PER_CYCLE;
    if (credit > SOURCE_GPU_T_CREDIT_LIMIT) credit = budget > SOURCE_GPU_T_CREDIT_LIMIT ? budget : SOURCE_GPU_T_CREDIT_LIMIT;
    return (int32_t)credit;
}

/* [TEST] command_projection:53,102 — a command starts at zero credit. */
#define SOURCE_GPU_T_ADMIT_AT 0
/* [TEST] command_projection:102-103 — charge for TEXPAGE, TEXWINDOW, FLUSHCACHE
 * and transfer set-up, which the documentation gives no time for. Drawing
 * commands are priced by the [DOC] tables alone; NOP and E3h-E6h cost nothing. */
#define SOURCE_GPU_T_COMMAND_OVERHEAD 2

/* ---- Polygons [DOC] "Polygons", New GPU -------------------------------------- */

static inline int source_gpu_t_shaded_or_textured(unsigned opcode) { return (opcode & 0x14u) != 0; }

/* Per triangle: precalc, plus 1.00 per any scanline of its Y span. */
#define SOURCE_GPU_T_POLYGON_TRIANGLE(t) source_gpu_t_polygon_triangle(t)
static inline int64_t source_gpu_t_polygon_triangle(const SourceGPUCostTally *t)
{
    int64_t q = 40;                                   /* 10.00 base precalc */
    if (t->opcode & 0x04u) q += 360;                  /* 90.00 textured */
    if (t->opcode & 0x10u) q += 600;                  /* 150.00 gouraud */
    return q + 4 * (int64_t)t->all_rows;              /* 1.00 per any-scanline */
}

/* Per drawn scanline of the given width. */
#define SOURCE_GPU_T_POLYGON_ROW(t, width) source_gpu_t_polygon_row((t), (width))
static inline int64_t source_gpu_t_polygon_row(const SourceGPUCostTally *t, int width)
{
    int rich = source_gpu_t_shaded_or_textured(t->opcode);
    int64_t q;
    if (!t->reads_back) {
        q = 4;                                        /* 1.00 per scanline */
        if (!rich) {
            static const int by_width[8] = { 0, 8, 6, 5, 3, 3, 2, 2 };
            q += width < 8 ? by_width[width] : 0;     /* 2.00/1.50/1.25/0.75/0.50/0 */
        }
    } else if (rich) {
        q = 8;                                        /* 2.00 per scanline */
    } else {
        q = 21 + 15 * (int64_t)((width + 15) / 16);   /* 5.25 + 3.75 per 16pix chunk */
    }
    int pixels = width & ~1;                          /* rounded to pixel pairs */
    return q + (rich ? 4 : 2) * (int64_t)pixels;      /* 1.00 or 0.50 per pixel */
}

/* ---- Rectangles [DOC] "Rectangles", New GPU ---------------------------------- */

#define SOURCE_GPU_T_SPRITE_ROW(t, width) source_gpu_t_sprite_row((t), (width))
static inline int64_t source_gpu_t_sprite_row(const SourceGPUCostTally *t, int width)
{
    int64_t q;
    if (t->reads_back) {
        q = 24 + 15 * (int64_t)((width + 15) / 16);   /* 6.00 + 3.75 per 16pix chunk */
    } else {
        q = width == 1 ? 14 : width <= 3 ? 10 : width <= 5 ? 8 : width <= 7 ? 6 : 4;
    }
    return q + 2 * (int64_t)(width & ~1);             /* 0.50 per pixel, pairs */
}
#define SOURCE_GPU_T_SPRITE_RECT(t) (4 * (int64_t)(t)->all_rows) /* 1.00 per any-scanline */

/* ---- Lines [DOC] "Lines", New GPU ------------------------------------------- */

#define SOURCE_GPU_T_LINE(t, dx, dy) source_gpu_t_line((t), (dx), (dy))
static inline int64_t source_gpu_t_line(const SourceGPUCostTally *t, int dx, int dy)
{
    int64_t q = 160;                                  /* 40.00 base precalc */
    if (t->opcode & 0x10u) q += 240;                  /* 60.00 gouraud */
    int pixels = (dx > dy ? dx : dy) + 1;
    q += (dy ? 8 : 4) * (int64_t)pixels;              /* 2.00 / 1.00 per pixel */
    int64_t per_row = dy >= dx && dy ? (t->reads_back ? 22 : 10) : 8; /* 5.50/2.50 steep, 2.00 */
    return q + per_row * t->drawn_rows;               /* offscreen scanlines 0.00 */
}

/* ---- Memory transfers [DOC] "Memory Transfers", New GPU ----------------------- */

static inline int64_t source_gpu_t_fill(unsigned width, unsigned height)
{
    if (!width) return 4 * (int64_t)height;           /* 1.00 per scanline, xsiz=0 */
    return 4 * (int64_t)(width / 16) * height + 20 * (int64_t)height; /* 1.00/16pix + 5.00 */
}
#define SOURCE_GPU_T_FILL(w, h) source_gpu_t_fill((w), (h))

static inline int64_t source_gpu_t_copy(unsigned width, unsigned height, int mask_check)
{
    if (!mask_check) return (5 * (int64_t)width + 78) * height;        /* 1.25/px + 19.50 */
    if (width < 16) return (6 * (int64_t)width + 89) * height;         /* 1.50/px + 22.25 */
    return (4 * (int64_t)width + 102 * (int64_t)((width + 15) / 16)) * height; /* 1.00/px + 25.50/chunk */
}
#define SOURCE_GPU_T_COPY(w, h, m) source_gpu_t_copy((w), (h), (m))

/* A0h and C0h: 1.00 per pixel, two pixels per word. */
#define SOURCE_GPU_T_TRANSFER_WORD(s) ((void)(s), 8)

/* ---- FIFO [DOC] No$PSX "GPU FIFO" (section extract 5767a2b3a5c8...) --------- */

/* "a 16-word (64-byte) write FIFO". An overrun fails closed here; the
 * hardware's overwrite-and-repeat behaviour ("FIFO Overrun") is not modelled. */
#define SOURCE_GPU_T_FIFO_WORDS 16u

/* "FIFO Prefetch": words the next command takes while a render is busy. */
#define SOURCE_GPU_T_PREFETCH_ATTRIBUTE 1u   /* NOP..MASKBITS, TEXPAGE..REFRESH */
#define SOURCE_GPU_T_PREFETCH_POLY_LINE 0u   /* POLY, LINE */
#define SOURCE_GPU_T_PREFETCH_RECT_SMALL 1u  /* RECT fixed size, without texture */
#define SOURCE_GPU_T_PREFETCH_RECT_LARGE 2u  /* RECT variable size, or with texture */
#define SOURCE_GPU_T_PREFETCH_FILL 2u        /* VRAM FILL */
#define SOURCE_GPU_T_PREFETCH_COPY 1u        /* VRAM-to-VRAM, CPU-to-VRAM, VRAM-to-CPU */

/* ---- Model scope [TEST] ------------------------------------------------------ */

/* The budget is this projection's own bookkeeping, not a hardware register.
 * GP1(00h)/(01h) clear the FIFO and abort the current command ([DOC] PSX-SPX
 * GP1 section); what happens to the credit is a model detail.
 * [TEST] verify_gpu_reset_projection.py — keeps positive credit, clears debt. */
#define SOURCE_GPU_T_RESET_CREDIT(b) ((b) < 0 ? 0 : (b))

/* [TEST] reset_projection:18-21; command_projection:80-83 — draw states the
 * model does not cover (interlaced drawing, 2 MB clip, PAL display). */
#define SOURCE_GPU_T_DRAW_REJECTED(s) \
    ((s)->clip_y0 > 511 || (s)->clip_y1 > 511 || source_gpu_command_interlaced(s))
#define SOURCE_GPU_T_DISPLAY_REJECTED(s) (((s)->display_mode & 0x08u) != 0)

#endif
