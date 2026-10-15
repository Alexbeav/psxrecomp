/* video_texture_filter_test: launcher texture-filter values to the enum. */
#include "video_texture_filter.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(void) {
    CHECK(psx_texture_filter_from_ui(0) == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_texture_filter_from_ui(1) == PSX_TEXTURE_FILTER_BILINEAR);
    CHECK(psx_texture_filter_from_ui(2) == PSX_TEXTURE_FILTER_STABLE_WORLD);
    CHECK(psx_texture_filter_from_ui(3) == PSX_TEXTURE_FILTER_XBR);  /* was collapsed to bilinear */
    CHECK(psx_texture_filter_from_ui(9) == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_texture_filter_from_ui(-1) == PSX_TEXTURE_FILTER_NEAREST);
    if (!fails) printf("video_texture_filter_test: ok\n");
    return fails ? 1 : 0;
}
