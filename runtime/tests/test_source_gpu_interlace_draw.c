/* PS1B-180: interlaced drawing is normal operation, not an unsupported state.
 *
 * Every SCPH550x boot draws its 480i logo background with this packet: GP0(28h),
 * a flat opaque quad in colour 000000 at (0,0) (640,0) (0,480) (640,480), while
 * GP1(08h)=27h (NTSC, 640 wide, 480 lines, interlace on) and GP0(E1h) bit 10 = 0.
 * The projection must dispatch both halves and charge only the rows of the
 * field being drawn. It still fails closed while no field is known.
 *
 * Rules: PSX-SPX a253f078 "GP1(08h) Display mode" (interlace affects GP0 draw
 * commands) and "GP0(E1h)" bit 10; No$PSX "GPU Status Register" bits 13, 31. */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static void check(int okay, const char *message)
{
    checks++;
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static const uint32_t boot_quad[5] = {
    0x28000000u, 0x00000000u, 0x00000280u, 0x01E00000u, 0x01E00280u
};

/* Cold projection in the boot's display and draw state. */
static void boot_state(SourceGPUCommandProjection *s, unsigned field_valid, uint32_t draw_mode_word)
{
    source_gpu_command_cold(s);
    s->field_valid = field_valid;
    s->skip_field = 0;
    check(source_gpu_command_gp1(s, 0x08000027u), "GP1(08h)=27h (NTSC 480i) accepted");
    check(source_gpu_command_write(s, 0xE3000000u), "drawing area top left");
    check(source_gpu_command_write(s, 0xE407FFFFu), "drawing area bottom right 1023,511");
    check(source_gpu_command_write(s, draw_mode_word), "draw mode queued");
    check(source_gpu_command_update(s, 100000), "credit for the attribute and the quad");
    check(!s->count && !s->error, "attributes applied");
}

int main(void)
{
    SourceGPUCommandProjection s;

    /* 1. The boot packet dispatches as a split quad, with no error. */
    boot_state(&s, 1, 0xE100020Au);
    check((s.draw_mode & 0x400u) == 0 && source_gpu_command_interlaced(&s), "480i, drawing to display area off");
    for (unsigned i = 0; i < 4; ++i) check(source_gpu_command_write(&s, boot_quad[i]), "boot quad words accepted");
    check(!s.error && s.dispatch.kind == SOURCE_GPU_DISPATCH_QUAD_FIRST && s.dispatch.count == 4,
          "first triangle dispatched, not rejected");
    check(source_gpu_command_update(&s, 200000), "credit for the second half");
    check(source_gpu_command_write(&s, boot_quad[4]), "fourth vertex accepted");
    if (s.dispatch.kind != SOURCE_GPU_DISPATCH_QUAD_SECOND)
        check(source_gpu_command_update(&s, 400000) && s.dispatch.kind == SOURCE_GPU_DISPATCH_QUAD_SECOND,
              "second triangle dispatched");
    check(!s.error && s.first_triangles == 1 && s.second_triangles == 1 && !s.phase, "both halves drawn");

    /* 2. Row skipping: each field costs less than drawing every row. */
    SourceGPUCommandProjection field0, field1, progressive;
    boot_state(&field0, 1, 0xE100020Au);
    field1 = field0; field1.skip_field = 1;
    progressive = field0; progressive.display_mode = 0x07u;   /* same mode, interlace off */
    for (int second = 0; second < 2; ++second) {
        int even = source_gpu_command_polygon_cost(&field0, boot_quad, second);
        int odd = source_gpu_command_polygon_cost(&field1, boot_quad, second);
        int all = source_gpu_command_polygon_cost(&progressive, boot_quad, second);
        check(even > 0 && odd > 0 && all > 0, "costs computed");
        check(even < all && odd < all, "interlaced draw skips the other field's rows");
    }

    /* 3. With E1h bit 10 set the model draws every row (PSX-SPX bit 10 = 1,
     * drawing to the display area allowed). */
    SourceGPUCommandProjection allowed;
    boot_state(&allowed, 1, 0xE100060Au);
    check(!source_gpu_command_interlaced(&allowed), "bit 10 set: no row skipping");
    check(source_gpu_command_polygon_cost(&allowed, boot_quad, 0) ==
          source_gpu_command_polygon_cost(&progressive, boot_quad, 0), "bit 10 set: full cost");

    /* 4. Fail closed while the caller has supplied no field. */
    boot_state(&s, 0, 0xE100020Au);
    for (unsigned i = 0; i < 3; ++i) check(source_gpu_command_write(&s, boot_quad[i]), "partial packet queues");
    check(!source_gpu_command_write(&s, boot_quad[3]) && s.error == SOURCE_GPU_COMMAND_UNSUPPORTED,
          "unknown field is still unsupported");

    printf("source_gpu_interlace_draw: %u checks passed\n", checks);
    return 0;
}
