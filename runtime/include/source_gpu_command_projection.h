/* GP0/GP1 command FIFO timing projection for the source GPU path.
 *
 * The projection accepts GP0 words, keeps them in a FIFO, and decides when
 * each command has all of its words and enough drawing credit to start. It
 * then publishes one dispatch record that tells the renderer what to draw.
 * It renders nothing itself.
 *
 * Structure source: PSX-SPX revision a253f078553f83b4e540f087abf0c28d954b6293,
 * docs/graphicsprocessingunitgpu.md: GPU Command Summary, Render Polygon /
 * Line / Rectangle Commands, Rendering Attributes (Vertex, E1h-E6h), Memory
 * Transfer Commands, Other Commands, GP1 Display Control Commands, GPU
 * Status Register (Ready Bits).
 *
 * No timing value appears in this file. Every cost, credit and threshold is
 * read by name from source_gpu_command_timing.h. The documentation defines
 * none of them; see evidence/A2i-spec-gaps.md (PS1B-166, gap G2).
 */
#ifndef PSX_SOURCE_GPU_COMMAND_PROJECTION_H
#define PSX_SOURCE_GPU_COMMAND_PROJECTION_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "source_gpu_polygon_projection.h"
#include "source_gpu_texture.h"

enum { SOURCE_GPU_DISPATCH_NONE, SOURCE_GPU_DISPATCH_COMMAND,
       SOURCE_GPU_DISPATCH_QUAD_FIRST, SOURCE_GPU_DISPATCH_QUAD_SECOND,
       SOURCE_GPU_DISPATCH_UPLOAD_WORD };

enum { SOURCE_GPU_COMMAND_UNSUPPORTED = 1, SOURCE_GPU_COMMAND_OVERFLOW = 2,
       SOURCE_GPU_COMMAND_REVERSE_TIME = 3 };

typedef struct SourceGPUCommandDispatch { unsigned kind, count; uint32_t words[12]; } SourceGPUCommandDispatch;

typedef struct SourceGPUCommandProjection {
    int32_t budget;
    uint32_t queue[32], count, phase, command;
    uint64_t last_update;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int offset_x, offset_y;
    uint32_t draw_mode, texture_window, mask_bits, display_mode, dma_direction;
    unsigned field_valid, skip_field;
    unsigned first_triangles, second_triangles;
    unsigned pline, pline_command;
    uint32_t pline_color, pline_vertex;
    uint32_t polygon_words[12];
    uint32_t transfer_words;
    SourceGPUCommandDispatch dispatch;
    int error;
} SourceGPUCommandProjection;

#if defined(__cplusplus)
#define PSX_GPUPP_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define PSX_GPUPP_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

/* The save-state wire and source_gpu_runtime.c copy these fields one by one. */
PSX_GPUPP_STATIC_ASSERT(sizeof(((SourceGPUCommandDispatch *)0)->words) == 12 * sizeof(uint32_t), "dispatch words");
PSX_GPUPP_STATIC_ASSERT(offsetof(SourceGPUCommandDispatch, words) == 2 * sizeof(unsigned), "dispatch layout");
PSX_GPUPP_STATIC_ASSERT(sizeof(((SourceGPUCommandProjection *)0)->queue) == 32 * sizeof(uint32_t), "queue words");
PSX_GPUPP_STATIC_ASSERT(offsetof(SourceGPUCommandProjection, queue) == sizeof(int32_t), "budget then queue");


/* Phase values. 0 = no command in progress. */
enum { SOURCE_GPU_PHASE_IDLE = 0, SOURCE_GPU_PHASE_QUAD_SECOND = 2,
       SOURCE_GPU_PHASE_UPLOAD = 4, SOURCE_GPU_PHASE_DOWNLOAD = 8 };

/* ---- Word decoding (PSX-SPX "GPU Command Summary", "Render ... Commands") */

static inline unsigned source_gpu_opcode(uint32_t word) { return word >> 24; }

/* Vertex word: X in bits 0-10, Y in bits 16-26, both signed 11-bit. */
static inline int source_gpu_command_coord(uint32_t word, unsigned shift)
{
    unsigned field = (word >> shift) & 0x7FFu;
    return (field & 0x400u) ? (int)field - 0x800 : (int)field;
}

static inline int source_gpu_sprite_origin(uint32_t word, unsigned shift, int offset)
{
    return source_gpu_command_coord(word, shift) + offset;
}

static inline int source_gpu_polygon_supported(unsigned opcode) { return (opcode & 0xE0u) == 0x20u; }
static inline int source_gpu_line_supported(unsigned opcode) { return (opcode & 0xE0u) == 0x40u; }
static inline int source_gpu_line_polyline(unsigned opcode) { return (opcode & 0x08u) != 0; }

/* Words per vertex: optional colour (gouraud, bit 28), the vertex, and an
 * optional UV word (textured, bit 26). */
static inline unsigned source_gpu_polygon_stride(unsigned opcode)
{
    return 1u + ((opcode >> 4) & 1u) + ((opcode >> 2) & 1u);
}

/* Poly-line: each further vertex is an optional colour word and a vertex. */
static inline unsigned source_gpu_line_segment_length(unsigned opcode)
{
    return 1u + ((opcode >> 4) & 1u);
}

/* PSX-SPX: the list ends on a word with (word AND F000F000h) = 50005000h. */
static inline int source_gpu_line_terminator(uint32_t word)
{
    return (word & 0xF000F000u) == 0x50005000u;
}

static inline int source_gpu_block_supported(unsigned opcode)
{
    return opcode == 0x02u || opcode == 0x80u || source_gpu_sprite_opcode(opcode);
}

/* Rectangle packet: command+colour, vertex, optional UV (bit 26), and a size
 * word only for the variable-size class. */
static inline unsigned source_gpu_sprite_length(unsigned opcode)
{
    return 2u + ((opcode >> 2) & 1u) + (source_gpu_sprite_class(opcode) == 0);
}

/* Words that must arrive before a command can start. For polygons and lines
 * this is the first three / two vertices (PSX-SPX "GPU Command Summary"); a
 * quad's fourth vertex and poly-line segments follow as a continuation. */
static inline unsigned source_gpu_command_length(uint32_t word)
{
    unsigned opcode = source_gpu_opcode(word);
    if (source_gpu_polygon_supported(opcode))
        return 1u + 3u * source_gpu_polygon_stride(opcode) - ((opcode >> 4) & 1u);
    if (source_gpu_line_supported(opcode))
        return 1u + 2u * source_gpu_line_segment_length(opcode) - ((opcode >> 4) & 1u);
    if (source_gpu_sprite_opcode(opcode)) return source_gpu_sprite_length(opcode);
    switch (opcode) {
    case 0x02: return 3u;          /* fill: colour, corner, size */
    case 0x80: return 4u;          /* VRAM copy: source, destination, size */
    case 0xA0: case 0xC0: return 3u; /* transfer: destination/source, size */
    default: return 1u;
    }
}

/* Timing values live in one table; see its header for their sources. */
#include "source_gpu_command_timing.h"

/* Words received before GPUSTAT.28 (ready for DMA block) drops. PSX-SPX
 * "Ready Bits": polygon and line commands drop it right after the command
 * word; every other command once the command and all its parameters arrive. */
static inline unsigned source_gpu_command_feedback_length(uint32_t word)
{
    unsigned opcode = source_gpu_opcode(word);
    if (source_gpu_polygon_supported(opcode) || source_gpu_line_supported(opcode)) return 1u;
    if (source_gpu_sprite_opcode(opcode)) return SOURCE_GPU_T_RECT_FEEDBACK(opcode);
    return source_gpu_command_length(word);
}

/* Commands this projection models. Everything else fails closed. */
static inline int source_gpu_command_known(unsigned opcode)
{
    if (source_gpu_polygon_supported(opcode) || source_gpu_line_supported(opcode) ||
        source_gpu_block_supported(opcode)) return 1;
    switch (opcode) {
    case 0x00: case 0x01: case 0xA0: case 0xC0:
    case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: return 1;
    default: return 0;
    }
}

/* Commands that execute on reaching the FIFO head without waiting for credit:
 * NOP, drawing area (E3h/E4h) and drawing offset (E5h). Oracle-observed: with
 * an empty FIFO these run at once even at negative credit, and cost nothing;
 * MASKBITS (E6h) queues and is charged like E1h. (No$PSX "GPU FIFO" lists
 * E6h with the immediate group; the oracle does not.) Words queued behind
 * another command stay in order. */
static inline int source_gpu_command_immediate(unsigned opcode)
{
    return opcode == 0x00u || (opcode >= 0xE3u && opcode <= 0xE5u);
}

/* No$PSX "GPU FIFO", FIFO Prefetch: words a command may take out of the FIFO
 * while a rendering command is still busy. */
static inline unsigned source_gpu_command_prefetch(uint32_t word)
{
    unsigned opcode = source_gpu_opcode(word);
    if (source_gpu_polygon_supported(opcode) || source_gpu_line_supported(opcode))
        return SOURCE_GPU_T_PREFETCH_POLY_LINE;
    if (source_gpu_sprite_opcode(opcode))
        return (source_gpu_sprite_class(opcode) && !(opcode & 4u)) ?
               SOURCE_GPU_T_PREFETCH_RECT_SMALL : SOURCE_GPU_T_PREFETCH_RECT_LARGE;
    if (opcode == 0x02u) return SOURCE_GPU_T_PREFETCH_FILL;
    if (opcode == 0x80u || opcode == 0xA0u || opcode == 0xC0u) return SOURCE_GPU_T_PREFETCH_COPY;
    return SOURCE_GPU_T_PREFETCH_ATTRIBUTE;
}

/* Fixed setup class of a polygon: 0 flat, 1 gouraud, 2 textured, 3 both. */
static inline unsigned source_gpu_polygon_setup(unsigned opcode)
{
    return ((opcode >> 4) & 1u) | (((opcode >> 2) & 1u) << 1);
}

/* ---- Rendering attributes (PSX-SPX E1h-E6h) ---------------------------- */

static inline void source_gpu_command_environment(SourceGPUCommandProjection *s, uint32_t word)
{
    switch (source_gpu_opcode(word)) {
    case 0xE1: s->draw_mode = word & 0x3FFFu; break;
    case 0xE2: s->texture_window = word & 0xFFFFFu; break;
    case 0xE3: s->clip_x0 = (int)(word & 0x3FFu); s->clip_y0 = (int)((word >> 10) & 0x3FFu); break;
    case 0xE4: s->clip_x1 = (int)(word & 0x3FFu); s->clip_y1 = (int)((word >> 10) & 0x3FFu); break;
    case 0xE5: s->offset_x = source_gpu_command_coord(word, 0);
               s->offset_y = source_gpu_command_coord(word, 11); break;
    case 0xE6: s->mask_bits = word & 3u; break;
    default: break;
    }
}

/* ---- Drawing work -------------------------------------------------------- */

static inline int source_gpu_command_interlaced(const SourceGPUCommandProjection *s)
{
    /* GP1(08h) bit 5 on with 480 lines (bit 2), and draw-to-display off. */
    return (s->display_mode & 0x24u) == 0x24u && !(s->draw_mode & 0x400u);
}

/* Semi-transparency and mask check both read the old pixel, and cost the
 * same (No$PSX "GPU Rendering Timings", "Semi-Transparency and Mask Check"). */
static inline int source_gpu_command_reads_back(const SourceGPUCommandProjection *s, unsigned opcode)
{
    return (opcode & 2u) || (s->mask_bits & 2u);
}


/* Polygon work: a fixed set-up charge per triangle half plus the scanline
 * walk of source_gpu_polygon_projection.h (pixels per drawn row, doubled for
 * gouraud or textured, plus the read-back share for semi-transparency or mask
 * check, and a charge per row outside the drawing area). */
static inline int source_gpu_command_polygon_cost(const SourceGPUCommandProjection *s,
                                                  const uint32_t *words, int second)
{
    unsigned opcode = source_gpu_opcode(words[0]), stride = source_gpu_polygon_stride(opcode);
    int x[3], y[3];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned v = i + (second ? 1u : 0u);
        x[i] = source_gpu_command_coord(words[1 + stride * v], 0) + s->offset_x;
        y[i] = source_gpu_command_coord(words[1 + stride * v], 16) + s->offset_y;
    }
    int work = source_poly_cost(x, y, s->clip_x0, s->clip_y0, s->clip_x1, s->clip_y1,
                                (opcode & 0x14u) != 0, source_gpu_command_reads_back(s, opcode),
                                source_gpu_command_interlaced(s), s->skip_field);
    if (work < 0) return -1;
    return SOURCE_GPU_T_POLYGON_SETUP(opcode, second) + work;
}

/* Rectangle work: set-up plus, per drawn row inside the drawing area, one unit
 * per pixel and, with read-back, one per aligned pixel pair touched. */
static inline int source_gpu_command_sprite_cost(const SourceGPUCommandProjection *s,
                                                 const uint32_t *words)
{
    unsigned opcode = source_gpu_opcode(words[0]), width, height;
    source_gpu_sprite_extent(opcode, words, &width, &height);
    int left = source_gpu_sprite_origin(words[1], 0, s->offset_x);
    int top = source_gpu_sprite_origin(words[1], 16, s->offset_y);
    int x0 = left > s->clip_x0 ? left : s->clip_x0;
    int x1 = left + (int)width - 1 < s->clip_x1 ? left + (int)width - 1 : s->clip_x1;
    int y0 = top > s->clip_y0 ? top : s->clip_y0;
    int y1 = top + (int)height - 1 < s->clip_y1 ? top + (int)height - 1 : s->clip_y1;
    int cost = SOURCE_GPU_T_SPRITE_SETUP;
    if (x1 < x0 || y1 < y0) return cost;
    int per_row = (x1 - x0 + 1) * SOURCE_GPU_T_SPRITE_PIXEL;
    if (source_gpu_command_reads_back(s, opcode))
        per_row += SOURCE_GPU_T_SPRITE_PAIR * ((x1 >> 1) - (x0 >> 1) + 1);
    for (int row = y0; row <= y1; ++row)
        if (!(source_gpu_command_interlaced(s) && ((unsigned)row & 1u) == s->skip_field))
            cost += per_row;
    return cost;
}
/* GP0(02h): PSX-SPX "Masking and Rounding for FILL Command parameters". */
static inline int source_gpu_command_fill_cost(const uint32_t *words)
{
    unsigned width = ((words[2] & 0x3FFu) + 0x0Fu) & ~0x0Fu;
    unsigned height = (words[2] >> 16) & 0x1FFu;
    if (!height) return 0;
    return SOURCE_GPU_T_FILL(width, height);
}

/* GP0(80h): PSX-SPX "Masking for COPY Commands parameters". */
static inline int source_gpu_command_copy_cost(const SourceGPUCommandProjection *s,
                                               const uint32_t *words)
{
    unsigned width = ((words[3] & 0xFFFFu) - 1u) % 0x400u + 1u;
    unsigned height = ((words[3] >> 16) - 1u) % 0x200u + 1u;
    return SOURCE_GPU_T_COPY(width, height, (s->mask_bits & 2u) != 0);
}

static inline int source_gpu_command_block_cost(const SourceGPUCommandProjection *s,
                                                const uint32_t *words)
{
    unsigned opcode = source_gpu_opcode(words[0]);
    if (opcode == 0x02u) return source_gpu_command_fill_cost(words);
    if (opcode == 0x80u) return source_gpu_command_copy_cost(s, words);
    return source_gpu_command_sprite_cost(s, words);
}

/* One line segment. Lines include both end points (PSX-SPX "Render Line"). */
static inline int source_gpu_command_line_cost(const SourceGPUCommandProjection *s,
                                               const uint32_t *words)
{
    unsigned opcode = source_gpu_opcode(words[0]), step = source_gpu_line_segment_length(opcode);
    int ax = source_gpu_command_coord(words[1], 0) + s->offset_x;
    int ay = source_gpu_command_coord(words[1], 16) + s->offset_y;
    int bx = source_gpu_command_coord(words[1 + step], 0) + s->offset_x;
    int by = source_gpu_command_coord(words[1 + step], 16) + s->offset_y;
    int dx = bx > ax ? bx - ax : ax - bx, dy = by > ay ? by - ay : ay - by;
    int top = ay < by ? ay : by, bottom = ay < by ? by : ay;
    int first = top > s->clip_y0 ? top : s->clip_y0;
    int last = bottom < s->clip_y1 ? bottom : s->clip_y1;
    return SOURCE_GPU_T_LINE(opcode, source_gpu_command_reads_back(s, opcode), dx, dy,
                             last >= first ? last - first + 1 : 0);
}

/* ---- FIFO ---------------------------------------------------------------- */

static inline uint32_t source_gpu_command_pop(SourceGPUCommandProjection *s)
{
    uint32_t word = s->queue[0];
    memmove(s->queue, s->queue + 1, (s->count - 1) * sizeof(s->queue[0]));
    s->queue[--s->count] = 0;
    return word;
}

static inline void source_gpu_command_cold(SourceGPUCommandProjection *s)
{
    memset(s, 0, sizeof(*s));
}

static inline int source_gpu_command_fail(SourceGPUCommandProjection *s, int code)
{
    s->error = code;
    return 0;
}

static inline void source_gpu_command_publish(SourceGPUCommandProjection *s, unsigned kind,
                                              const uint32_t *words, unsigned count)
{
    s->dispatch.kind = kind;
    s->dispatch.count = count;
    memcpy(s->dispatch.words, words, count * sizeof(words[0]));
}

/* Words the rest of the queue still owes: the unfinished tail packet. */
static inline unsigned source_gpu_command_owed(const SourceGPUCommandProjection *s)
{
    unsigned at = 0, owed = 0;
    if (s->phase == SOURCE_GPU_PHASE_QUAD_SECOND) {
        unsigned stride = source_gpu_polygon_stride(s->command);
        at = s->count < stride ? s->count : stride;
        owed = stride - at;
    }
    while (at < s->count) {
        unsigned length = source_gpu_command_length(s->queue[at]);
        owed = at + length > s->count ? at + length - s->count : 0;
        at += length;
    }
    return owed;
}

/* Words still occupying the 16-word FIFO. While drawing is busy, the next
 * command at the head may already have taken its prefetch words out
 * (No$PSX "GPU FIFO", FIFO Prefetch). */
static inline unsigned source_gpu_command_fifo_size(const SourceGPUCommandProjection *s)
{
    if (!s->count || s->budget >= 0 || s->pline || s->phase != SOURCE_GPU_PHASE_IDLE)
        return s->count;
    unsigned taken = source_gpu_command_prefetch(s->queue[0]);
    return s->count - (taken < s->count ? taken : s->count);
}

/* ---- Command execution --------------------------------------------------- */

static inline int source_gpu_command_draw_rejected(const SourceGPUCommandProjection *s)
{
    return SOURCE_GPU_T_DRAW_REJECTED(s);
}

static inline int source_gpu_command_start_polygon(SourceGPUCommandProjection *s)
{
    unsigned opcode = source_gpu_opcode(s->queue[0]);
    unsigned length = source_gpu_command_length(s->queue[0]);
    if (source_gpu_command_draw_rejected(s)) return source_gpu_command_fail(s, SOURCE_GPU_COMMAND_UNSUPPORTED);
    for (unsigned i = 0; i < length; ++i) s->polygon_words[i] = source_gpu_command_pop(s);
    /* PSX-SPX "Texpage Attribute": the second UV word's upper half sets
     * GP0(E1h) bits 0-8 and 11 for textured polygons. */
    if (opcode & 0x04u) {
        uint32_t page = s->polygon_words[2 + source_gpu_polygon_stride(opcode)] >> 16;
        s->draw_mode = (s->draw_mode & ~0x9FFu) | (page & 0x9FFu);
    }
    int cost = source_gpu_command_polygon_cost(s, s->polygon_words, 0);
    if (cost < 0) return source_gpu_command_fail(s, SOURCE_GPU_COMMAND_UNSUPPORTED);
    s->budget -= cost;
    s->command = opcode;
    s->first_triangles++;
    if (opcode & 0x08u) {
        s->phase = SOURCE_GPU_PHASE_QUAD_SECOND;
        source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_QUAD_FIRST, s->polygon_words, length);
    } else {
        s->phase = SOURCE_GPU_PHASE_IDLE;
        source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_COMMAND, s->polygon_words, length);
    }
    return 1;
}

static inline int source_gpu_command_finish_quad(SourceGPUCommandProjection *s)
{
    unsigned stride = source_gpu_polygon_stride(s->command);
    unsigned length = source_gpu_command_length(s->command << 24);
    for (unsigned i = 0; i < stride; ++i) s->polygon_words[length + i] = source_gpu_command_pop(s);
    int cost = source_gpu_command_polygon_cost(s, s->polygon_words, 1);
    if (cost < 0) return source_gpu_command_fail(s, SOURCE_GPU_COMMAND_UNSUPPORTED);
    s->budget -= cost;
    s->second_triangles++;
    s->phase = SOURCE_GPU_PHASE_IDLE;
    source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_QUAD_SECOND, s->polygon_words, length + stride);
    return 1;
}

static inline int source_gpu_command_start_line(SourceGPUCommandProjection *s)
{
    unsigned opcode = source_gpu_opcode(s->queue[0]);
    unsigned length = source_gpu_command_length(s->queue[0]);
    uint32_t words[4];
    for (unsigned i = 0; i < length; ++i) words[i] = source_gpu_command_pop(s);
    s->budget -= SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_line_cost(s, words);
    s->command = opcode;
    if (source_gpu_line_polyline(opcode)) {
        s->pline = 1;
        s->pline_command = opcode;
        s->pline_vertex = words[length - 1];
        s->pline_color = ((opcode & 0x10u) ? words[length - 2] : words[0]) & 0xFFFFFFu;
    }
    source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_COMMAND, words, length);
    return 1;
}

/* One further poly-line vertex: joins the previous vertex to this one. */
static inline int source_gpu_command_line_segment(SourceGPUCommandProjection *s)
{
    unsigned opcode = s->pline_command, step = source_gpu_line_segment_length(opcode);
    if (s->count < step) return 0;
    uint32_t words[4];
    unsigned n = 0;
    words[n++] = (opcode << 24) | s->pline_color;
    words[n++] = s->pline_vertex;
    for (unsigned i = 0; i < step; ++i) words[n++] = source_gpu_command_pop(s);
    if (opcode & 0x10u) s->pline_color = words[2] & 0xFFFFFFu;
    s->pline_vertex = words[n - 1];
    s->budget -= source_gpu_command_line_cost(s, words);
    source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_COMMAND, words, n);
    return 1;
}

static inline int source_gpu_command_start_block(SourceGPUCommandProjection *s)
{
    unsigned opcode = source_gpu_opcode(s->queue[0]);
    unsigned length = source_gpu_command_length(s->queue[0]);
    uint32_t words[4];
    for (unsigned i = 0; i < length; ++i) words[i] = source_gpu_command_pop(s);
    s->budget -= SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_block_cost(s, words);
    s->command = opcode;
    source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_COMMAND, words, length);
    return 1;
}

/* A0h and C0h: the size word gives width and height in halfwords; the data
 * phase moves ceil(width * height / 2) words (PSX-SPX "Memory Transfer"). */
static inline uint32_t source_gpu_command_transfer_words(uint32_t size)
{
    uint32_t width = ((size & 0xFFFFu) - 1u) & 0x3FFu;
    uint32_t height = ((size >> 16) - 1u) & 0x1FFu;
    uint32_t halfwords = (width + 1u) * (height + 1u);
    return (halfwords + 1u) / 2u;
}

static inline int source_gpu_command_start_other(SourceGPUCommandProjection *s)
{
    unsigned opcode = source_gpu_opcode(s->queue[0]);
    unsigned length = source_gpu_command_length(s->queue[0]);
    uint32_t words[3];
    for (unsigned i = 0; i < length; ++i) words[i] = source_gpu_command_pop(s);
    s->budget -= SOURCE_GPU_T_COMMAND_OVERHEAD;
    s->command = opcode;
    source_gpu_command_environment(s, words[0]);
    if (opcode == 0xA0u || opcode == 0xC0u) {
        s->transfer_words = source_gpu_command_transfer_words(words[2]);
        s->phase = opcode == 0xA0u ? SOURCE_GPU_PHASE_UPLOAD : SOURCE_GPU_PHASE_DOWNLOAD;
        if (!s->transfer_words) s->phase = SOURCE_GPU_PHASE_IDLE;
    }
    source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_COMMAND, words, length);
    return 1;
}

/* An attribute that executes on reaching the FIFO head, busy or not. It takes
 * no drawing time. */
static inline int source_gpu_command_run_immediate(SourceGPUCommandProjection *s, uint32_t word)
{
    source_gpu_command_environment(s, word);
    source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_COMMAND, &word, 1);
    return 1;
}

static inline int source_gpu_command_at_boundary(const SourceGPUCommandProjection *s)
{
    return !s->pline && s->phase == SOURCE_GPU_PHASE_IDLE;
}

/* Run the head of the queue once, if it is complete and credit allows. */
static inline int source_gpu_command_process(SourceGPUCommandProjection *s)
{
    if (s->count && source_gpu_command_at_boundary(s) &&
        source_gpu_command_immediate(source_gpu_opcode(s->queue[0])))
        return source_gpu_command_run_immediate(s, source_gpu_command_pop(s));
    if (!s->count || s->budget < SOURCE_GPU_T_ADMIT_AT) return 0;
    if (s->phase == SOURCE_GPU_PHASE_UPLOAD) {
        uint32_t word = source_gpu_command_pop(s);
        s->budget -= SOURCE_GPU_T_UPLOAD_WORD;
        source_gpu_command_publish(s, SOURCE_GPU_DISPATCH_UPLOAD_WORD, &word, 1);
        if (!--s->transfer_words) s->phase = SOURCE_GPU_PHASE_IDLE;
        return 1;
    }
    if (s->phase == SOURCE_GPU_PHASE_QUAD_SECOND) {
        if (s->count < source_gpu_polygon_stride(s->command)) return 0;
        return source_gpu_command_finish_quad(s);
    }
    if (s->pline) {
        if (source_gpu_line_terminator(s->queue[0])) {
            s->pline = 0;
            s->count = 0;
            memset(s->queue, 0, sizeof(s->queue));
            return 0;
        }
        return source_gpu_command_line_segment(s);
    }
    if (s->count < source_gpu_command_length(s->queue[0])) return 0;
    unsigned opcode = source_gpu_opcode(s->queue[0]);
    if (source_gpu_polygon_supported(opcode)) return source_gpu_command_start_polygon(s);
    if (source_gpu_line_supported(opcode)) return source_gpu_command_start_line(s);
    if (source_gpu_block_supported(opcode)) return source_gpu_command_start_block(s);
    return source_gpu_command_start_other(s);
}

/* ---- Public entry points ------------------------------------------------- */

static inline int source_gpu_command_write(SourceGPUCommandProjection *s, uint32_t word)
{
    if (s->error) return 0;
    s->dispatch.kind = SOURCE_GPU_DISPATCH_NONE;
    int starts_packet = source_gpu_command_at_boundary(s) && !source_gpu_command_owed(s);
    if (starts_packet) {
        unsigned opcode = source_gpu_opcode(word);
        if (!source_gpu_command_known(opcode))
            return source_gpu_command_fail(s, SOURCE_GPU_COMMAND_UNSUPPORTED);
        if (source_gpu_command_immediate(opcode) && !s->count)
            return source_gpu_command_run_immediate(s, word);
    }
    if (source_gpu_command_fifo_size(s) >= SOURCE_GPU_T_FIFO_WORDS)
        return source_gpu_command_fail(s, SOURCE_GPU_COMMAND_OVERFLOW);
    s->queue[s->count++] = word;
    source_gpu_command_process(s);
    return !s->error;
}

static inline int source_gpu_command_update(SourceGPUCommandProjection *s, uint64_t cycle)
{
    if (s->error) return 0;
    if (cycle < s->last_update) return source_gpu_command_fail(s, SOURCE_GPU_COMMAND_REVERSE_TIME);
    s->dispatch.kind = SOURCE_GPU_DISPATCH_NONE;
    s->budget = SOURCE_GPU_T_CREDIT(s, cycle - s->last_update);
    s->last_update = cycle;
    source_gpu_command_process(s);
    return !s->error;
}

/* GPUSTAT.28 (oracle-observed). During A0h and C0h data phases it is "write
 * FIFO empty". While a quad's second half or a poly-line is pending it is 0.
 * When idle it is 1 while fewer words are queued than the head command's
 * threshold (timing table). Drawing credit is not consulted. No$PSX "Ready
 * Bits" words the idle rule as clearing while a command executes. */
static inline int source_gpu_command_ready(const SourceGPUCommandProjection *s)
{
    if (s->phase == SOURCE_GPU_PHASE_UPLOAD || s->phase == SOURCE_GPU_PHASE_DOWNLOAD)
        return source_gpu_command_fifo_size(s) == 0;
    if (s->pline || s->phase != SOURCE_GPU_PHASE_IDLE) return 0;
    if (!s->count) return 1;
    return s->count < SOURCE_GPU_T_READY_BELOW(s->queue[0]);
}

/* GPUREAD during a C0h transfer consumes one data word. No time passes. */
static inline void source_gpu_command_read(SourceGPUCommandProjection *s)
{
    if (s->phase != SOURCE_GPU_PHASE_DOWNLOAD || !s->transfer_words) return;
    s->budget -= SOURCE_GPU_T_READ_WORD;
    if (!--s->transfer_words) s->phase = SOURCE_GPU_PHASE_IDLE;
}

static inline void source_gpu_command_clear(SourceGPUCommandProjection *s)
{
    memset(s->queue, 0, sizeof(s->queue));
    s->count = 0;
    s->phase = SOURCE_GPU_PHASE_IDLE;
    s->command = 0;
    s->pline = 0;
    s->transfer_words = 0;
    s->dispatch.kind = SOURCE_GPU_DISPATCH_NONE;
    s->budget = SOURCE_GPU_T_RESET_CREDIT(s->budget);
}

/* PSX-SPX GP1(00h), (01h), (04h), (08h). Other GP1 commands leave this
 * projection unchanged. */
static inline int source_gpu_command_gp1(SourceGPUCommandProjection *s, uint32_t word)
{
    switch (word >> 24 & 0x3Fu) {
    case 0x00:
        source_gpu_command_clear(s);
        s->draw_mode = s->texture_window = s->mask_bits = 0;
        s->clip_x0 = s->clip_y0 = s->clip_x1 = s->clip_y1 = 0;
        s->offset_x = s->offset_y = 0;
        s->display_mode = 0;
        s->dma_direction = 0;
        return 1;
    case 0x01:
        source_gpu_command_clear(s);
        return 1;
    case 0x04:
        s->dma_direction = word & 3u;
        return 1;
    case 0x08:
        s->display_mode = word & 0xFFu;
        return !SOURCE_GPU_T_DISPLAY_REJECTED(s);
    default:
        return 1;
    }
}

#endif /* PSX_SOURCE_GPU_COMMAND_PROJECTION_H */
