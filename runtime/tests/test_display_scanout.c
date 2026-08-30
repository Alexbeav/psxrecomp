#include "display_scanout.h"

#include <assert.h>

static void test_vertical_offsets(void)
{
    assert(psx_display_vertical_offset(1, 83, 312) == 32);
    assert(psx_display_vertical_offset(1, 35, 291) == 0);
    assert(psx_display_vertical_offset(0, 16, 256) == 0);
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

int main(void)
{
    test_vertical_offsets();
    test_row_shift();
    return 0;
}
