/* Accepted authored source-profile raster, ac56b624; see GPU-SOURCE-PROFILE-RASTER-SPEC provenance. */
/* T172 authored texture cache control. */
typedef struct {
    uint32_t address;
    uint16_t words[4];
    int valid;
} T172TextureLine;
static T172TextureLine t172_texture_lines[256];
static uint16_t t172_palette[256];
static unsigned t172_palette_key, t172_palette_size;
static int t172_palette_valid;
static uint32_t t172_control_page;

void sw_source_texture_control(unsigned action, uint32_t page)
{
    uint32_t selected = page & 0x19fu;
    /* FFIX 1549904: inline 15-bit -> 8-bit selection at the same physical
     * page retains source bitmap cache lines (public TexCache observation).
     * The 4-bit layout is distinct: FFIX 1551147 source cache snapshots
     * invalidate across 4-bit <-> 8-bit. Preserve explicit controls 0..4. */
    if (action == SOURCE_GPU_TEXTURE_POLYGON_PAGE) {
        if (!((selected ^ t172_control_page) & 0x1fu) &&
            (selected == t172_control_page ||
             ((selected & 0x180u) && (t172_control_page & 0x180u)))) {
            t172_control_page = selected;
            return;
        }
        action = 3;
    }
    if (action == 3 && selected == t172_control_page) return;
    for (unsigned i = 0; i < 256; ++i) t172_texture_lines[i].valid = 0;
    if (action == 0 || action == 1 || action == 4) t172_palette_valid = 0;
    if (action == 0) {
        for (unsigned i = 0; i < 256; ++i) t172_palette[i] = 0;
    }
    if (action == 0) t172_control_page = 0;
    if (action == 3 || action == 4) t172_control_page = selected;
}

/* Source profile: the state of one draw call. Written from the behaviour spec
 * (recomp-corpus references/ps1/GPU-SOURCE-PROFILE-RASTER-SPEC.md); "spec"
 * below is that note and the row IDs are its own.
 * Spec 5 I6: the kept texture helpers below read `texture` and `mode` and add
 * to `extra_work`. The other fields belong to the pixel rule (spec 6.1) and to
 * the triangle set-up (spec 6.2 T5). Spec 5 I8: one of these lives on the
 * stack of each call; nothing carries from one call to the next. */
typedef struct SourceTriangleColors {
    int extra_work,mode;
    const SourceGPUTexture *texture;
    int dither;                 /* spec 6.1 P6: dither is on for this primitive */
    /* Spec 6.2 T5, per channel (red, green, blue, U, V): the value at the
     * anchor vertex times 4096 plus 2048, and the two slopes. All sums with
     * them are formed modulo 2^32. */
    uint32_t base[5],slope_x[5],slope_y[5];
    int anchor_x,anchor_y;      /* spec 6.2 T4 */
} SourceTriangleColors;
/* T172 authored texture sampling. */
static uint16_t source_texture_fetch(SourceTriangleColors *colors, unsigned u, unsigned v)
{
    const SourceGPUTexture *texture = colors->texture;
    unsigned mask_u = (texture->window & 31u) << 3;
    unsigned mask_v = ((texture->window >> 5) & 31u) << 3;
    u = (u & ~mask_u) | (((texture->window >> 10) << 3) & mask_u);
    v = (v & ~mask_v) | (((texture->window >> 15) << 3) & mask_v);
    unsigned shift = 2u - (unsigned)colors->mode;
    unsigned x = ((texture->page & 15u) * 64u + (u >> shift)) & 1023u;
    unsigned y = (((texture->page >> 4) & 1u) * 256u + v) & 511u;
    unsigned slot = colors->mode == 0 ? ((v & 63u) * 4u + ((u >> 4) & 3u))
        : ((v & 31u) * 8u + ((u >> (shift + 2)) & 7u));
    uint32_t address = y * 1024u + (x & ~3u);
    T172TextureLine *line = &t172_texture_lines[slot];
    if (!line->valid || line->address != address) {
        line->valid = 1;
        line->address = address;
        for (unsigned i = 0; i < 4; ++i) line->words[i] = g_vram[address + i];
        colors->extra_work += 4;
    }
    uint16_t word = line->words[x & 3u];
    if (colors->mode == 2) return word;
    unsigned bits = colors->mode == 0 ? 4u : 8u;
    unsigned index = (word >> ((u & ((1u << shift) - 1u)) * bits)) & ((1u << bits) - 1u);
    return t172_palette[index];
}

static void source_texture_palette(SourceTriangleColors *colors)
{
    const SourceGPUTexture *texture = colors->texture;
    if (colors->mode == 2 || !texture->load_clut) return;
    unsigned size = colors->mode == 0 ? 16u : 256u;
    unsigned key = texture->clut & 0x7fffu;
    if (t172_palette_valid && t172_palette_key == key && t172_palette_size == size) return;
    unsigned x = (key & 63u) * 16u;
    unsigned y = (key >> 6) & 511u;
    for (unsigned i = 0; i < size; ++i) t172_palette[i] = g_vram[y * 1024u + ((x + i) & 1023u)];
    t172_palette_key = key;
    t172_palette_size = size;
    t172_palette_valid = 1;
    colors->extra_work += (int)size;
}

/* Spec 5 I7: the texture depth is bits 7-8 of the page word; 0 is 4-bit, 1 is
 * 8-bit, 2 is 15-bit, and 3 counts as 2 (PSX-SPX "GP0(E1h)": "Reserved" is
 * the same as 15bit). */
static inline int source_texture_mode(unsigned page)
{
    unsigned mode = (page >> 7) & 3u;
    return mode == 3u ? 2 : (int)mode;
}

/* PSX-SPX "24bit RGB to 15bit RGB Dithering": the offsets of one 4 x 4 block,
 * [row][column]. */
static const int8_t source_dither_offset[4][4] = {
    { -4,  0, -3,  1 },
    {  2, -2,  3, -1 },
    { -3,  1, -4,  0 },
    {  3, -1,  2, -2 },
};

/* Spec 6.1, the pixel rule of the source profile: one pixel of a triangle, a
 * line or a sprite. (x, row) is the store position, y the interpolation row;
 * r, g, b are the 8-bit channel values and u, v the texture coordinate at the
 * pixel. Returns 1 when the pixel was stored. */
static int source_put_pixel(SourceTriangleColors *s, int x, int row, int y,
                            unsigned r, unsigned g, unsigned b, unsigned u, unsigned v)
{
    const SourceGPUTexture *texture = s->texture;
    uint16_t texel = 0, pixel;
    if (texture) {
        /* P2: the texel is fetched for every pixel, before any other test;
         * the order of the fetches decides the texture work [UNIT:
         * tas_gpu_textured_triangle, tas_gpu_polygon_order]. P3: a texel of
         * 0000h draws nothing (PSX-SPX "Texture Bitmaps"). */
        texel = source_texture_fetch(s, u, v);
        if (!texel) return 0;
    }
    /* P1: the store address is row modulo 512, times 1024, plus x. Every
     * caller clips x to a drawing area inside VRAM; an address outside the
     * array stores nothing. */
    int64_t address = (int64_t)(((unsigned)row & 511u) * 1024u) + x;
    if (address < 0 || address >= VRAM_WIDTH * VRAM_HEIGHT) return 0;
    if (texture && texture->raw) {
        pixel = texel;                                    /* P4 */
    } else {
        const unsigned channel[3] = { r, g, b };
        pixel = texel & 0x8000u;                          /* P7: bit 15 of the texel, or 0 */
        for (unsigned i = 0; i < 3; ++i) {
            int value = (int)channel[i];
            /* P5: PSX-SPX "Modulation (also known as Texture Blending)". */
            if (texture) value = (int)((((unsigned)texel >> (5u * i)) & 31u) * channel[i]) >> 4;
            /* P6: the table row is the interpolation row, the column the
             * store column [FIXTURE: T-shaded, T-family]. */
            if (s->dither) value += source_dither_offset[(unsigned)y & 3u][(unsigned)x & 3u];
            if (value < 0) value = 0;                     /* P7 */
            if (value > 255) value = 255;
            pixel |= (uint16_t)((unsigned)(value >> 3) << (5u * i));
        }
    }
    uint16_t *destination = &g_vram[address];
    /* P8: PSX-SPX "GP0(E6h)". The test comes after the fetch and after P3. */
    if (g_mask_check_bit && (*destination & 0x8000u)) return 0;
    /* P9: PSX-SPX "Semi-transparency". A textured pixel blends only when bit
     * 15 of its texel is set, and then keeps that bit. */
    if (g_semi_trans_enabled && (!texture || (texel & 0x8000u))) {
        pixel = blend_pixels(*destination, pixel, g_semi_trans_mode);
        if (texture) pixel |= 0x8000u;
    }
    if (g_mask_set_bit) pixel |= 0x8000u;                 /* P10 */
    *destination = pixel;
    return 1;
}

/* Spec 6.2 T6: the span callback of source_poly_walk. The walk gives the row,
 * the first store column, the width and the first interpolation column. The
 * columns are drawn in ascending order; store column and interpolation column
 * step together. T5, T5b: the channel values are taken at the interpolation
 * column and at the walk's row, neither wrapped. */
static void source_triangle_span(void *context, int raw_y, int physical_x,
                                 int width, int raw_interpolation_x)
{
    SourceTriangleColors *s = (SourceTriangleColors *)context;
    uint32_t value[5];
    int stored = 0;
    for (unsigned c = 0; c < 5; ++c)
        value[c] = s->base[c]
                 + s->slope_x[c] * ((uint32_t)raw_interpolation_x - (uint32_t)s->anchor_x)
                 + s->slope_y[c] * ((uint32_t)raw_y - (uint32_t)s->anchor_y);
    for (int i = 0; i < width; ++i) {
        /* T5: floor(sum / 4096) modulo 256 is bits 12 to 19 of the sum. */
        stored |= source_put_pixel(s, physical_x + i, raw_y, raw_y,
                                   (value[0] >> 12) & 255u, (value[1] >> 12) & 255u,
                                   (value[2] >> 12) & 255u, (value[3] >> 12) & 255u,
                                   (value[4] >> 12) & 255u);
        for (unsigned c = 0; c < 5; ++c) value[c] += s->slope_x[c];
    }
    if (stored) gpu_vram_dirty_mark_row((uint32_t)raw_y & 511u);   /* spec 5 I9 */
}

/* Spec 5 I1, I2, I4; 6.2 T1-T7: one triangle of the source profile. */
int sw_draw_source_triangle(const int *x,const int *y,const uint32_t *colors,
                            int shaded,int dither,int interlace,unsigned skip_field,
                            const SourceGPUTexture *texture,int *extra_work) {
    *extra_work=0;
    if(g_hr || g_wide_cur || g_precise_valid || g_perspective_valid)return 0;   /* I4 */
    SourceTriangleColors s;
    memset(&s, 0, sizeof(s));
    s.texture = texture;
    if (texture) {
        /* T1: the palette comes first, also for a triangle that draws nothing
         * [UNIT: tas_gpu_textured_triangle]. */
        s.mode = source_texture_mode(texture->page);
        source_texture_palette(&s);
    }
    /* T2: PSX-SPX "24bit RGB to 15bit RGB Dithering": polygons are dithered
     * only with gouraud shading or modulation. */
    s.dither = dither && (shaded || texture);

    /* T3: the signed area. The differences are formed in 64 bits and the
     * products modulo 2^64, so that no coordinate value overflows; the walk
     * rejects every triangle whose area would not fit. */
    int64_t edge1_x = (int64_t)x[1] - x[0], edge1_y = (int64_t)y[1] - y[0];
    int64_t edge2_x = (int64_t)x[2] - x[0], edge2_y = (int64_t)y[2] - y[0];
    int64_t area = (int64_t)((uint64_t)edge1_x * (uint64_t)edge2_y
                           - (uint64_t)edge2_x * (uint64_t)edge1_y);
    if (area) {
        /* T4: the anchor is the vertex with the least x. On a tie vertex 1
         * wins over 0, 2 over 1, and 0 over 2 [FIXTURE: T-order]. */
        int anchor = x[1] <= x[0] ? 1 : 0;
        if (x[2] < x[anchor] || (x[2] == x[anchor] && anchor == 1)) anchor = 2;
        s.anchor_x = x[anchor];
        s.anchor_y = y[anchor];
        for (unsigned c = 0; c < 5; ++c) {
            int at[3];
            for (unsigned i = 0; i < 3; ++i)
                at[i] = (int)(c < 3 ? (colors[i] >> (8u * c)) & 255u
                                    : texture ? (texture->uv[i] >> (8u * (c - 3u))) & 255u : 0u);
            s.base[c] = (uint32_t)at[anchor] * 4096u + 2048u;
            /* T5a: the colour of an unshaded triangle has no slope. */
            if (c < 3 && !shaded) continue;
            /* T5: the slopes of the plane through the three vertices, in
             * 1/4096 units, each truncated. */
            int64_t d1 = at[1] - at[0], d2 = at[2] - at[0];
            s.slope_x[c] = (uint32_t)((d1 * edge2_y - d2 * edge1_y) * 4096 / area);
            s.slope_y[c] = (uint32_t)((edge1_x * d2 - edge2_x * d1) * 4096 / area);
        }
    }
    /* T6: the pixels come from the walk. */
    int work = source_poly_walk(x, y, g_clip_x1, g_clip_y1, g_clip_x2, g_clip_y2,
                                shaded || texture, g_mask_check_bit || g_semi_trans_enabled,
                                interlace, skip_field, source_triangle_span, &s);
    *extra_work = s.extra_work;                           /* T7 */
    return work < 0 ? 0 : 1;
}

/* PSX-SPX "Vertex (Parameter for Polygon, Line, Rectangle commands)": a signed
 * 11-bit coordinate at bit `shift` of a vertex word. */
static inline int source_vertex_coordinate(uint32_t word, unsigned shift)
{
    int field = (int)((word >> shift) & 0x7FFu);
    return field >= 0x400 ? field - 0x800 : field;
}

/* Spec 6.3 L1-L8: one segment of the line family (opcodes 40h to 5Fh). */
static int source_draw_line(const SourceGPUBlock *block)
{
    const uint32_t *words = block->words;
    unsigned shaded = (words[0] >> 28) & 1u;              /* L1: bit 4 of the opcode */
    uint32_t first = words[1], second = words[shaded ? 3 : 2];
    uint32_t color[2];
    color[0] = words[0] & 0xFFFFFFu;
    color[1] = shaded ? words[2] & 0xFFFFFFu : color[0];
    /* L2: PSX-SPX "Vertex": lines whose vertices are 1024 or more apart in x,
     * or 512 or more in y, are not drawn. */
    int dx = source_vertex_coordinate(second, 0) - source_vertex_coordinate(first, 0);
    int dy = source_vertex_coordinate(second, 16) - source_vertex_coordinate(first, 16);
    int across = dx < 0 ? -dx : dx, down = dy < 0 ? -dy : dy;
    if (across >= 1024 || down >= 512) return 1;
    int count = across > down ? across : down;            /* L3: N; the line has N + 1 points */
    /* The start point. Positions are taken modulo 2048 (L5), so unsigned
     * arithmetic gives them for every value of block->x and block->y. */
    unsigned start_x = (unsigned)block->x, start_y = (unsigned)block->y;
    if (count && dx <= 0) {
        /* L4 [FIXTURE: G1]: with dx below 0, or dx 0 and dy not 0, the line
         * is walked from the second vertex, and the colours change places. */
        start_x += (unsigned)dx;
        start_y += (unsigned)dy;
        dx = -dx;
        dy = -dy;
        uint32_t other = color[0];
        color[0] = color[1];
        color[1] = other;
    }
    SourceTriangleColors s;
    memset(&s, 0, sizeof(s));
    /* L6: PSX-SPX "GP0(E1h)" bit 9; lines are dithered, shaded or not. */
    s.dither = (int)((block->draw_mode >> 9) & 1u);
    int from[3], step[3];
    for (unsigned c = 0; c < 3; ++c) {
        /* L7 [FIXTURE: G1]: the step of a channel is (last - first) * 4096 /
         * N, truncated; an unshaded line has both ends equal. */
        from[c] = (int)((color[0] >> (8u * c)) & 255u);
        step[c] = count ? ((int)((color[1] >> (8u * c)) & 255u) - from[c]) * 4096 / count : 0;
    }
    for (int i = 0; i <= count; ++i) {                    /* L8: in the order i = 0..N */
        /* L5 [FIXTURE: G1]: point i lies at start + i * delta / N, rounded
         * to the nearest integer. dx is not negative here. A tie in x goes
         * to the lower x; a tie in y goes away from the start row. */
        unsigned move_x = count ? (unsigned)((2 * i * dx + count - 1) / (2 * count)) : 0u;
        unsigned move_y = count ? (unsigned)((2 * i * down + count) / (2 * count)) : 0u;
        int px = (int)((start_x + move_x) & 2047u);
        int py = (int)((dy < 0 ? start_y - move_y : start_y + move_y) & 2047u);
        /* L6: the drawing area of the block, and the skipped field. */
        if (px < block->clip_left || px > block->clip_right) continue;
        if (py < block->clip_top || py > block->clip_bottom) continue;
        if (block->interlace && ((unsigned)py & 1u) == block->skip_field) continue;
        /* L7: floor((first * 4096 + 2048 + i * step) / 4096); the sum is
         * never negative. */
        if (source_put_pixel(&s, px, py, py,
                             (unsigned)((from[0] * 4096 + 2048 + i * step[0]) >> 12) & 255u,
                             (unsigned)((from[1] * 4096 + 2048 + i * step[1]) >> 12) & 255u,
                             (unsigned)((from[2] * 4096 + 2048 + i * step[2]) >> 12) & 255u, 0, 0))
            gpu_vram_dirty_mark_row((uint32_t)py & 511u);   /* spec 5 I9 */
    }
    return 1;
}

/* Spec 6.4 C1-C5: the VRAM-to-VRAM copy (opcode 80h). */
static int source_copy_vram(const uint32_t *words)
{
    /* C1: PSX-SPX "GP0(80h)" and "Masking for COPY Commands parameters". */
    unsigned from_x = words[1] & 1023u, from_y = (words[1] >> 16) & 511u;
    unsigned to_x = words[2] & 1023u, to_y = (words[2] >> 16) & 511u;
    unsigned width = words[3] & 1023u, height = (words[3] >> 16) & 511u;
    if (!width) width = 1024u;
    if (!height) height = 512u;
    /* C2: rows in ascending order; x wraps at 1024 and y at 512; no drawing
     * area and no skipped field. */
    for (unsigned row = 0; row < height; ++row) {
        const uint16_t *source = g_vram + ((from_y + row) & 511u) * 1024u;
        uint16_t *target = g_vram + ((to_y + row) & 511u) * 1024u;
        /* C3 [FIXTURE: G2]: a row moves in runs of 128 pixels. A run is read
         * whole before any of it is written. */
        for (unsigned done = 0; done < width; done += 128u) {
            uint16_t run[128];
            unsigned length = width - done < 128u ? width - done : 128u;
            for (unsigned i = 0; i < length; ++i) run[i] = source[(from_x + done + i) & 1023u];
            for (unsigned i = 0; i < length; ++i) {
                uint16_t *destination = &target[(to_x + done + i) & 1023u];
                /* C4: PSX-SPX "GP0(E6h)" applies to the copy. */
                if (g_mask_check_bit && (*destination & 0x8000u)) continue;
                *destination = g_mask_set_bit ? (uint16_t)(run[i] | 0x8000u) : run[i];
            }
        }
        gpu_vram_dirty_mark_row((to_y + row) & 511u);       /* spec 5 I9 */
    }
    return 1;                                             /* C5 */
}

/* Spec 6.5 R1-R7: one sprite (opcodes 60h to 7Fh). */
static int source_draw_sprite(const SourceGPUBlock *block, unsigned opcode, int *extra_work)
{
    const uint32_t *words = block->words;
    unsigned width, height, u0 = 0, v0 = 0;
    source_gpu_sprite_extent(opcode, words, &width, &height);   /* R1 */
    /* R2: the command colour at every pixel; sprites are never dithered. */
    unsigned r = words[0] & 255u, g = (words[0] >> 8) & 255u, b = (words[0] >> 16) & 255u;
    SourceGPUTexture texture;
    SourceTriangleColors s;
    memset(&texture, 0, sizeof(texture));
    memset(&s, 0, sizeof(s));
    if (opcode & 4u) {
        /* R3: the page comes from the draw mode and the palette word from
         * word 2. The palette is loaded once, before the clip, also when
         * nothing is drawn [UNIT: tas_gpu_sprite_blend]. */
        texture.page = (uint16_t)(block->draw_mode & 0x1FFu);
        texture.window = block->texture_window;
        texture.clut = (uint16_t)(words[2] >> 16);
        texture.raw = (int)(opcode & 1u);
        texture.load_clut = 1;
        s.texture = &texture;
        s.mode = source_texture_mode(texture.page);
        source_texture_palette(&s);
        u0 = words[2] & 255u;                             /* R4 */
        v0 = (words[2] >> 8) & 255u;
    }
    /* R5: PSX-SPX "Texture Origin and X/Y-Flip"; "GP0(E1h)" bits 12, 13. */
    unsigned flip_x = (block->draw_mode >> 12) & 1u, flip_y = (block->draw_mode >> 13) & 1u;
    /* R6: the columns and rows inside the drawing area of the block.
     * Coordinates are not wrapped here. */
    int64_t left = block->x, top = block->y;
    int64_t first_x = left > block->clip_left ? left : block->clip_left;
    int64_t last_x = left + (int64_t)width - 1 < block->clip_right ? left + (int64_t)width - 1 : block->clip_right;
    int64_t first_y = top > block->clip_top ? top : block->clip_top;
    int64_t last_y = top + (int64_t)height - 1 < block->clip_bottom ? top + (int64_t)height - 1 : block->clip_bottom;
    if (first_x <= last_x) {
        for (int64_t row = first_y; row <= last_y; ++row) {
            if (block->interlace && ((unsigned)row & 1u) == block->skip_field) continue;
            /* R4, R5: U and V count from the unclipped origin, modulo 256.
             * A flipped U starts at (U0 or 1) [FIXTURE: G3]. */
            unsigned down = (unsigned)(row - top);
            unsigned v = (flip_y ? v0 - down : v0 + down) & 255u;
            int stored = 0;
            for (int64_t column = first_x; column <= last_x; ++column) {
                unsigned across = (unsigned)(column - left);
                unsigned u = (flip_x ? (u0 | 1u) - across : u0 + across) & 255u;
                stored |= source_put_pixel(&s, (int)column, (int)row, (int)row, r, g, b, u, v);
            }
            if (stored) gpu_vram_dirty_mark_row((uint32_t)row & 511u);   /* spec 5 I9 */
        }
    }
    *extra_work = s.extra_work;                           /* R7 */
    return 1;
}

int sw_draw_source_block(const SourceGPUBlock *block,int *extra_work) {
    *extra_work=0;
    if(g_hr || g_wide_cur || g_precise_valid || g_perspective_valid)return 0;
    const uint32_t *words=block->words;unsigned opcode=words[0]>>24;
    if(opcode>=0x40 && opcode<=0x5f)return source_draw_line(block);   /* spec 6.3 */
    if(opcode==2) {
        unsigned x0=words[1]&1008u,y0=(words[1]>>16)&511u;
        unsigned width=((words[2]&1023u)+15u)&~15u,height=(words[2]>>16)&511u;
        uint16_t color=((words[0]>>3)&31u)|((words[0]>>6)&992u)|((words[0]>>9)&31744u);
        for(unsigned row=0;row<height;row++) {
            unsigned y=(y0+row)&511u;
            if(block->interlace && (y&1u)==block->skip_field)continue;
            for(unsigned col=0;col<width;col++)g_vram[y*1024u+((x0+col)&1023u)]=color;
            if(width)gpu_vram_dirty_mark_row(y);
        }
        return 1;
    }
    if(opcode==0x80)return source_copy_vram(words);                   /* spec 6.4 */
    if(!source_gpu_sprite_opcode(opcode))return 0;                    /* spec 6.5 R7 */
    return source_draw_sprite(block,opcode,extra_work);               /* spec 6.5 */
}


#define SOURCE_TEXTURE_WIRE_BYTES 4624u
static int source_texture_wire(uint8_t *out,const uint8_t *in,uint32_t len,int apply){
 T172TextureLine lines[256];uint16_t palette[256];uint32_t key,size,valid,page;PstR r;PstW w;
 if(len!=SOURCE_TEXTURE_WIRE_BYTES)return 0;
 if(out){pst_w_init(&w,out,len);for(unsigned i=0;i<256;++i){pst_w_u32(&w,t172_texture_lines[i].address);for(unsigned j=0;j<4;++j)pst_w_u16(&w,t172_texture_lines[i].words[j]);pst_w_u32(&w,(uint32_t)t172_texture_lines[i].valid);}for(unsigned i=0;i<256;++i)pst_w_u16(&w,t172_palette[i]);pst_w_u32(&w,t172_palette_key);pst_w_u32(&w,t172_palette_size);pst_w_u32(&w,(uint32_t)t172_palette_valid);pst_w_u32(&w,t172_control_page);return 1;}
 if(!in)return 0;pst_r_init(&r,in,len);
 for(unsigned i=0;i<256;++i){uint32_t v;if(!pst_r_u32(&r,&lines[i].address))return 0;for(unsigned j=0;j<4;++j)if(!pst_r_u16(&r,&lines[i].words[j]))return 0;if(!pst_r_u32(&r,&v)||v>1||lines[i].address>0x7ffffu)return 0;lines[i].valid=(int)v;}
 for(unsigned i=0;i<256;++i)if(!pst_r_u16(&r,&palette[i]))return 0;
 if(!pst_r_u32(&r,&key)||!pst_r_u32(&r,&size)||!pst_r_u32(&r,&valid)||!pst_r_u32(&r,&page)||key>0xffffu||(size!=0&&size!=16&&size!=256)||valid>1||(page&~0x19fu))return 0;
 if(apply){memcpy(t172_texture_lines,lines,sizeof lines);memcpy(t172_palette,palette,sizeof palette);t172_palette_key=key;t172_palette_size=size;t172_palette_valid=(int)valid;t172_control_page=page;}return 1;
}
uint32_t sw_source_texture_snapshot_bytes(void){return SOURCE_TEXTURE_WIRE_BYTES;}
void sw_source_texture_snapshot_write(uint8_t *p){(void)source_texture_wire(p,0,SOURCE_TEXTURE_WIRE_BYTES,0);}
int sw_source_texture_snapshot_read(const uint8_t *p,uint32_t n,int apply){return source_texture_wire(0,p,n,apply);}
