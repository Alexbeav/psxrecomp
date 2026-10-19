#include "present_image_ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    uint8_t pixels[PRESENT_IMAGE_RING_H * 4];
    uint8_t *rgb = NULL;
    uint32_t lo, hi, frame;
    int w, h, count;
    memset(pixels, 0, sizeof pixels);
    for (int y = 0; y < PRESENT_IMAGE_RING_H; y++) pixels[4*y] = 255;
    present_image_ring_push(42, pixels, 1, 4, 0);
    for (int y = 0; y < PRESENT_IMAGE_RING_H; y++) {
        pixels[4*y] = 0; pixels[4*y+1] = 255;
    }
    present_image_ring_push(42, pixels, 1, 4, 0);
    present_image_ring_sequence_span(&lo, &hi);
    CHECK(lo == 1 && hi == 2);
    CHECK(present_image_ring_get_sequence_rgb(1, &rgb, &w, &h, &frame));
    CHECK(frame == 42 && w == 1 && h == PRESENT_IMAGE_RING_H);
    CHECK(rgb[0] == 248 && rgb[1] == 0); free(rgb);
    CHECK(present_image_ring_get_sequence_rgb(2, &rgb, &w, &h, &frame));
    CHECK(frame == 42 && rgb[0] == 0 && rgb[1] == 252); free(rgb);
    present_image_ring_set_frozen(1);
    present_image_ring_push(43, pixels, 1, 4, 0);
    present_image_ring_sequence_span(&lo, &hi);
    CHECK(hi == 2);
    present_image_ring_set_frozen(0);
    for (int i = 0; i < PRESENT_IMAGE_RING_CAP; i++)
        present_image_ring_push(43, pixels, 1, 4, 0);
    present_image_ring_sequence_span(&lo, &hi);
    CHECK(lo == 3 && hi == PRESENT_IMAGE_RING_CAP + 2);
    CHECK(!present_image_ring_get_sequence_rgb(2, &rgb, &w, &h, &frame));
    present_image_ring_span(&lo, &hi, &count);
    CHECK(lo == 43 && hi == 43 && count == PRESENT_IMAGE_RING_CAP);
    CHECK(present_image_ring_get_rgb(43, &rgb, &w, &h)); free(rgb);
    puts("presentation ring: duplicate frames, freeze and eviction verified");
    return 0;
}
