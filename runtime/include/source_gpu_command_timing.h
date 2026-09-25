/* Source GPU command timing table (TAS/source mode).
 *
 * Policy (Alex, 2026-09-25, PS1B-182): in TAS/source mode GPU timing and
 * readiness follow the TAS oracle. Every value below is [ORACLE]: fitted to
 * the PS1B-182 oracle GPU logs (Z:/Share/psxrecomp/evidence/T172/
 * ps1b-182-oracle-gpu-logs-20260925/), observed behaviour only, and it
 * reproduces every logged charge of its class exactly. The route and row
 * counts behind each value are given next to it. The No$PSX "GPU Rendering
 * Timings" / "GPU FIFO" value (sha256 a3b2131f3774..., 5767a2b3a5c8...) is
 * quoted beside it for comparison.
 *
 * [ORACLE FIXTURE] marks a value fitted to oracle outputs for authored inputs
 * that live in runtime/tests (not to route logs); check those fixtures'
 * provenance. [NOT OBSERVED] marks a rule no log or fixture exercises yet; it
 * keeps the documented behaviour until data exists.
 *
 * Budget unit: half a CPU clock (credit is 2 per CPU cycle).
 */
#ifndef PSX_SOURCE_GPU_COMMAND_TIMING_H
#define PSX_SOURCE_GPU_COMMAND_TIMING_H

/* ---- Credit -------------------------------------------------------------------
 * [ORACLE] +2 per elapsed CPU cycle, capped at 256 (T3 route-05: 7,092 rows;
 * MMX5 route-03: 14,567,011 rows; no mismatch). No$PSX: rendering runs on the
 * 33 MHz CPU clock; 0.50/0.25-clock steps "may rely on the GPU being clocked
 * at twice 33MHz". */
#define SOURCE_GPU_T_CREDIT_PER_CYCLE 2
#define SOURCE_GPU_T_CREDIT_LIMIT 256
#define SOURCE_GPU_T_CREDIT(s, elapsed) source_gpu_t_credit((s)->budget, (elapsed))
static inline int32_t source_gpu_t_credit(int32_t budget, uint64_t elapsed)
{
    int64_t credit = (int64_t)budget + (int64_t)elapsed * SOURCE_GPU_T_CREDIT_PER_CYCLE;
    if (credit > SOURCE_GPU_T_CREDIT_LIMIT) credit = budget > SOURCE_GPU_T_CREDIT_LIMIT ? budget : SOURCE_GPU_T_CREDIT_LIMIT;
    return (int32_t)credit;
}

/* [ORACLE] a queued command starts once credit is >= 0, one command per
 * service call (T3, MMX5 W/D/C rows). */
#define SOURCE_GPU_T_ADMIT_AT 0

/* [ORACLE] charge for 01h, E1h, E2h, E6h, A0h/C0h set-up (MMX5: 801,826 E1h,
 * 10,624 E2h, 10,630 E6h, 11,759 01h, 11,756 A0h, 3 C0h), and the command part
 * of rectangles, lines, fills and copies. NOP and E3h-E5h cost 0 (MMX5:
 * 57,380 / 20,933 each). No$PSX gives no time for attributes. */
#define SOURCE_GPU_T_COMMAND_OVERHEAD 2

/* ---- Polygons -------------------------------------------------------------------
 * [ORACLE] set-up per triangle half; the rest is the scanline walk of
 * source_gpu_polygon_projection.h. MMX5 residuals are constant per class:
 *   first half: flat 84 (8,914), textured 264 (180,584), gouraud 372 (14,076)
 *   second half: flat 46 (296), textured 226 (180,584), gouraud 334 (13,716)
 * Semi-transparency does not change the set-up (textured 35,911; gouraud 2,220).
 * Gouraud+textured: extra 450, not 180+288 [ORACLE FIXTURE: all 1,536
 * cases of source_gpu_shaded_texture_family_fixtures.json, both halves].
 * No$PSX precalc: 10 base, +90 textured, +150 gouraud clocks (= 20/180/300
 * half-clocks). The textured extra (180) matches No$PSX; base and gouraud differ. */
#define SOURCE_GPU_T_POLY_FIRST 84
#define SOURCE_GPU_T_POLY_SECOND 46
#define SOURCE_GPU_T_POLY_TEXTURED 180
#define SOURCE_GPU_T_POLY_GOURAUD 288
#define SOURCE_GPU_T_POLY_GOURAUD_TEXTURED 450
#define SOURCE_GPU_T_POLYGON_SETUP(op, second) \
    (((second) ? SOURCE_GPU_T_POLY_SECOND : SOURCE_GPU_T_POLY_FIRST) + \
     ((((op) & 0x14u) == 0x14u) ? SOURCE_GPU_T_POLY_GOURAUD_TEXTURED : \
      (((op) & 0x04u) ? SOURCE_GPU_T_POLY_TEXTURED : 0) + (((op) & 0x10u) ? SOURCE_GPU_T_POLY_GOURAUD : 0)))

/* ---- Rectangles ------------------------------------------------------------------
 * [ORACLE] command charge 2 + set-up 16 + per drawn row: 1 per pixel, and with
 * semi-transparency 1 per aligned pixel pair (MMX5: 4,125,621 rects, all five
 * observed classes constant residual 18). Texture and CLUT cache work is the
 * renderer's own charge (dispatch sink), logged separately.
 * No$PSX New GPU: per scanline 1.00-3.50 by width, 0.50 per pixel (pairs). */
#define SOURCE_GPU_T_SPRITE_SETUP 16
#define SOURCE_GPU_T_SPRITE_PIXEL 1
#define SOURCE_GPU_T_SPRITE_PAIR 1
/* [ORACLE] DMA feedback threshold of a rectangle at the FIFO head: 2, +1 if
 * textured, +1 (OR) if variable size (MMX5 GPUSTAT: 0x7D ready at 1-2 words,
 * not at 3; 0x62/0x7F not ready at 3). */
#define SOURCE_GPU_T_RECT_FEEDBACK(op) \
    (2u | (((op) >> 2) & 1u) | (source_gpu_sprite_class(op) == 0 ? 1u : 0u))

/* ---- Lines ----------------------------------------------------------------------
 * [ORACLE] Abe's route-05: 2,930 openings and 3,284 poly-line segments, all
 * gouraud and semi-transparent, exact. [ORACLE FIXTURE] flat and opaque lines:
 * the oracle's outputs for the 512 authored single-line cases in
 * runtime/tests/source_gpu_line_fixtures.json fit exactly: 16 per segment plus
 * 2 per step of the major axis, or 16 alone when the segment is too long to
 * draw (dx >= 1024 or dy >= 512, PSX-SPX "Vertex" size limit). Semi-transparency,
 * mask check, clip and interlace field do not change it. The opening segment
 * also pays the command charge; later poly-line segments do not
 * (test_source_gpu_line.c; confirmed by the Abe's segments).
 * No$PSX New GPU: 40 (+60 gouraud) clocks precalc, 1 per pixel horizontal,
 * 2 otherwise, 2-5.5 per scanline. */
#define SOURCE_GPU_T_LINE_SETUP 16
#define SOURCE_GPU_T_LINE_STEP 2
static inline int source_gpu_t_line(unsigned op, int reads_back, int dx, int dy, int drawn_rows)
{
    (void)op; (void)reads_back; (void)drawn_rows;
    if (dx >= 1024 || dy >= 512) return SOURCE_GPU_T_LINE_SETUP;
    return SOURCE_GPU_T_LINE_SETUP + SOURCE_GPU_T_LINE_STEP * (dx > dy ? dx : dy);
}
#define SOURCE_GPU_T_LINE(op, rb, dx, dy, rows) source_gpu_t_line((op), (rb), (dx), (dy), (rows))
/* ---- Fill and copy ------------------------------------------------------------------
 * Copy [ORACLE]: command charge 2 plus 2 per pixel, w*h after the PSX-SPX
 * size masking (MMX5 2x1 = 6, 10,298 rows; Abe's 192x240 = 92,162, 8 rows;
 * 384x240 = 184,322, 188 rows). Mask check: [NOT OBSERVED], same rule assumed.
 * No$PSX New GPU: 1.25 clocks per pixel + 19.5 per row without mask check.
 *
 * Fill [NOT FITTED]: every logged fill so far is 320x240 and costs 11,808
 * (MMX5, 10,298 rows). One size cannot fix a formula, so fill keeps No$PSX New
 * GPU in half-clocks (2 per 16 px + 10 per row) and does not match yet. */
#define SOURCE_GPU_T_COPY_PIXEL 2
static inline int source_gpu_t_fill(unsigned width, unsigned height)
{
    return (int)((width / 16u) * 2u * height + 10u * height);
}
static inline int source_gpu_t_copy(unsigned width, unsigned height, int mask_check)
{
    (void)mask_check;
    return (int)(SOURCE_GPU_T_COPY_PIXEL * width * height);
}
#define SOURCE_GPU_T_FILL(w, h) source_gpu_t_fill((w), (h))
#define SOURCE_GPU_T_COPY(w, h, m) source_gpu_t_copy((w), (h), (m))
/* [ORACLE] A0h data words cost 0 (MMX5: 23,265,848 words). C0h GPUREAD words:
 * [NOT OBSERVED] (3 C0h commands, no charged read); kept at 0. No$PSX: 1.00
 * clock per pixel either way. */
#define SOURCE_GPU_T_UPLOAD_WORD 0
#define SOURCE_GPU_T_READ_WORD 0

/* ---- Readiness -------------------------------------------------------------------
 * [ORACLE] GPUSTAT.28 while idle: 1 while fewer words are queued than the head
 * command's threshold. MMX5 GPUSTAT runs: NOP 1 (never ready with it queued),
 * 01h/E1h-E2h/E6h 2 (E1h ready at 1, not at 2), rectangles as above, polygons
 * and lines 1 (never ready once queued), fill/copy/transfers their packet
 * length (not ready once complete; shorter counts [NOT OBSERVED]).
 * Credit is not consulted (MMX5: ready at negative credit with an empty FIFO,
 * 4,472,544 reads). No$PSX: "Write FIFO empty". */
static inline unsigned source_gpu_t_ready_below(uint32_t head)
{
    unsigned op = head >> 24;
    if (op == 0x00u) return 1u;
    if (op == 0x01u || op == 0xE1u || op == 0xE2u || op == 0xE6u) return 2u;
    if (source_gpu_sprite_opcode(op)) return SOURCE_GPU_T_RECT_FEEDBACK(op);
    if ((op & 0xE0u) == 0x20u || (op & 0xE0u) == 0x40u) return 1u;
    return source_gpu_command_length(head);
}
#define SOURCE_GPU_T_READY_BELOW(head) source_gpu_t_ready_below(head)

/* ---- FIFO [NOT OBSERVED beyond depth 10] -------------------------------------------
 * No$PSX "GPU FIFO": 16 words; while drawing is busy the head command may take
 * its prefetch words out. The deepest oracle FIFO seen is 10 words (MMX5). */
#define SOURCE_GPU_T_FIFO_WORDS 16u
#define SOURCE_GPU_T_PREFETCH_ATTRIBUTE 1u
#define SOURCE_GPU_T_PREFETCH_POLY_LINE 0u
#define SOURCE_GPU_T_PREFETCH_RECT_SMALL 1u
#define SOURCE_GPU_T_PREFETCH_RECT_LARGE 2u
#define SOURCE_GPU_T_PREFETCH_FILL 2u
#define SOURCE_GPU_T_PREFETCH_COPY 1u

/* ---- Reset and model scope -----------------------------------------------------------
 * GP1(00h)/(01h) clear the FIFO and abort the current command (PSX-SPX).
 * [ORACLE] credit after reset: positive credit kept, debt cleared (G rows). */
#define SOURCE_GPU_T_RESET_CREDIT(b) ((b) < 0 ? 0 : (b))

/* Draw states the model does not cover: 2 MB clip, PAL display, and
 * interlaced drawing before the caller supplies a field (PS1B-180). */
#define SOURCE_GPU_T_DRAW_REJECTED(s) \
    ((s)->clip_y0 > 511 || (s)->clip_y1 > 511 || \
     (source_gpu_command_interlaced(s) && !(s)->field_valid))
#define SOURCE_GPU_T_DISPLAY_REJECTED(s) (((s)->display_mode & 0x08u) != 0)

#endif
