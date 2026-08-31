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
}

static void test_vertical_layout(void)
{
    PsxDisplayVerticalLayout alundra = psx_display_vertical_layout(1, 83, 312);
    assert(alundra.valid);
    assert(alundra.canvas_height == 288);
    assert(alundra.canvas_origin_y == 63);
    assert(alundra.source_skip_y == 0);
    assert(alundra.source_height == 225);
    assert(alundra.offset_y == 32);

    PsxDisplayVerticalLayout clipped_top = psx_display_vertical_layout(1, 0, 100);
    assert(clipped_top.valid);
    assert(clipped_top.canvas_origin_y == 0);
    assert(clipped_top.source_skip_y == 20);
    assert(clipped_top.source_height == 80);

    assert(!psx_display_vertical_layout(1, 400, 500).valid);
    assert(!psx_display_vertical_layout(0, 100, 100).valid);
}

int main(void)
{
    test_vertical_offsets();
    test_vertical_layout();
    test_present_height();
    return 0;
}
