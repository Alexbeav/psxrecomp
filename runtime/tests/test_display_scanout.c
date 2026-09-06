#include "display_scanout.h"

#include <assert.h>

static void test_vertical_offsets(void)
{
    assert(psx_display_vertical_offset(1, 83, 312) == 32);
    assert(psx_display_vertical_offset(1, 35, 291) == 0);
    assert(psx_display_vertical_offset(0, 16, 256) == 0);
    assert(psx_display_vertical_offset(1, 100, 100) == 0);
    assert(psx_display_vertical_offset(0, 200, 100) == 0);
    assert(psx_display_vertical_offset(1, 400, 500) == 0);
}

static void test_present_height(void)
{
    assert(psx_display_present_height(0, 225, 288) == 225);
    assert(psx_display_present_height(1, 225, 288) == 288);
    assert(psx_display_present_height(1, 225, 0) == 225);
    assert(psx_display_interlaced_rows(288, 0) == 288);
    assert(psx_display_interlaced_rows(288, 1) ==
           PSX_DISPLAY_PRESENT_MAX_HEIGHT);
    assert(psx_display_clip_source_height(0, 450, 512, 126) == 450);
    assert(psx_display_clip_source_height(1, 450, 512, 126) == 386);
    assert(psx_display_clip_source_height(1, 450, 512, 512) == 0);

    PsxDisplayVerticalLayout unset = psx_display_vertical_layout(0, 100, 100);
    assert(!unset.range_set);
    assert(psx_display_source_height(unset, 240) == 240);

    PsxDisplayVerticalLayout offscreen = psx_display_vertical_layout(1, 400, 500);
    assert(offscreen.range_set);
    assert(!offscreen.valid);
    assert(offscreen.canvas_height == 288);
    assert(psx_display_source_height(offscreen, 240) == 0);
}

static void test_vertical_layout(void)
{
    PsxDisplayVerticalLayout offset_range = psx_display_vertical_layout(1, 83, 312);
    assert(offset_range.valid);
    assert(offset_range.canvas_height == 288);
    assert(offset_range.canvas_origin_y == 63);
    assert(offset_range.source_skip_y == 0);
    assert(offset_range.source_height == 225);
    assert(offset_range.offset_y == 32);

    PsxDisplayVerticalLayout clipped_top = psx_display_vertical_layout(1, 0, 100);
    assert(clipped_top.valid);
    assert(clipped_top.canvas_origin_y == 0);
    assert(clipped_top.source_skip_y == 20);
    assert(clipped_top.source_height == 80);

    assert(!psx_display_vertical_layout(1, 400, 500).valid);
    assert(!psx_display_vertical_layout(0, 100, 100).valid);
}

static void test_row_shift(void)
{
    uint32_t down[5] = { 1, 2, 3, 4, 5 };
    uint32_t up[5] = { 1, 2, 3, 4, 5 };
    const uint32_t black = 0xFF000000u;

    psx_display_shift_rows_argb(down, 1, 5, 1);
    assert(down[0] == black && down[1] == 1 && down[4] == 4);

    psx_display_shift_rows_argb(up, 1, 5, -2);
    assert(up[0] == 3 && up[2] == 5 && up[3] == black && up[4] == black);
}

static void assert_depth24_composition(int pal, uint32_t y1, uint32_t y2)
{
    const uint32_t black = 0xFF000000u;
    uint32_t canvas[288];
    PsxDisplayVerticalLayout layout = psx_display_vertical_layout(pal, y1, y2);

    assert(layout.valid);
    assert(layout.canvas_height <= 288u);
    for (uint32_t row = 0; row < layout.canvas_height; row++)
        canvas[row] = black;

    uint32_t mapped = 0u;
    for (uint32_t row = 0; row < layout.source_height; row++) {
        uint32_t canvas_y = 0u;
        uint32_t source_y = 0u;
        assert(psx_display_stage_depth24_row(
            layout.canvas_height, layout.canvas_origin_y,
            layout.source_skip_y, layout.source_height, row,
            &canvas_y, &source_y));
        canvas[canvas_y] = source_y + 1u;
        mapped++;
    }

    assert(mapped == layout.source_height);
    for (uint32_t row = 0; row < layout.canvas_origin_y; row++)
        assert(canvas[row] == black);
    for (uint32_t row = 0; row < layout.source_height; row++)
        assert(canvas[layout.canvas_origin_y + row] ==
               layout.source_skip_y + row + 1u);
    for (uint32_t row = layout.canvas_origin_y + layout.source_height;
         row < layout.canvas_height; row++)
        assert(canvas[row] == black);
}

static void test_depth24_composition(void)
{
    /* Mortal Kombat Trilogy PAL FMV range. */
    assert_depth24_composition(1, 83, 312);
    /* A centred NTSC movie range with a non-zero canvas origin. */
    assert_depth24_composition(0, 24, 232);
}

int main(void)
{
    test_vertical_offsets();
    test_vertical_layout();
    test_present_height();
    test_row_shift();
    test_depth24_composition();
    return 0;
}
