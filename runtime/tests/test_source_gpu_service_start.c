/* PS1B-182: when the projection may start a queued command.
 *
 * [ORACLE] MMX5 route-03 at cycle 950,194,816: E1h + 7Dh dispatch as their
 * words arrive, then the next E1h + 7Dh (four words, complete) queue at
 * budget -282. The oracle starts the queued E1h at the service step at
 * 950,195,072 (budget 228) and the already-complete 7Dh only at the next
 * elapsed step, 950,195,200 (256 -> -18). A second service call at the same
 * cycle (own tick and DMA tick coincide) starts nothing.
 *
 * Also: a queued, not yet started quad still owes its fourth vertex group, so
 * those words are parameters, not new commands (MMX5 1,500,460,160: E1h and a
 * nine-word 2Dh quad queue at budget -118 to a FIFO count of 10). */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

int main(void)
{
    SourceGPUCommandProjection s;
    source_gpu_command_cold(&s);
    s.clip_x0 = 0; s.clip_y0 = 240; s.clip_x1 = 319; s.clip_y1 = 479;
    s.offset_x = 0; s.offset_y = 240; s.draw_mode = 0x26; s.field_valid = 1;
    s.budget = -282;
    s.last_update = 950194816u;
    const uint32_t queued[4] = { 0xE1000026u, 0x7D000000u, 0x00BC0060u, 0x78011090u };
    for (unsigned i = 0; i < 4; ++i) check(source_gpu_command_write(&s, queued[i]), "queued words accepted");
    check(s.count == 4 && s.budget == -282, "E1h + 7Dh queued at negative credit");

    check(source_gpu_command_update(&s, 950194944u) && s.budget == -26 && s.count == 4, "no start below zero credit");
    check(source_gpu_command_update(&s, 950195072u), "service at 950,195,072");
    check(s.dispatch.kind == SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.words[0] == 0xE1000026u &&
          s.budget == 228 && s.count == 3, "E1h starts, budget 228");
    check(source_gpu_command_update(&s, 950195072u), "same-cycle service accepted");
    check(s.dispatch.kind == SOURCE_GPU_DISPATCH_NONE && s.count == 3 && s.budget == 228,
          "same-cycle service starts nothing: 7Dh waits");
    check(source_gpu_command_update(&s, 950195200u), "service at 950,195,200");
    check(s.dispatch.kind == SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.words[0] == 0x7D000000u &&
          s.count == 0 && s.budget == 256 - 274, "7Dh starts at the next elapsed step, 256 -> -18");

    /* Queued quad owes its fourth vertex group. */
    source_gpu_command_cold(&s);
    s.clip_x1 = 319; s.clip_y0 = 240; s.clip_y1 = 479; s.field_valid = 1;
    s.budget = -118;
    const uint32_t quad[10] = { 0xE1000020u, 0x2DFFFFFFu, 0xFFC80078u, 0x7800000Fu, 0xFFC80087u,
                                0x00250000u, 0xFFD80078u, 0x00FF100Fu, 0xFFD80087u, 0x00101000u };
    for (unsigned i = 0; i < 10; ++i) {
        check(source_gpu_command_write(&s, quad[i]), "queued quad word accepted");
        check(s.count == i + 1, "queued quad word stays in the FIFO");
    }
    check(!s.error, "fourth vertex group is not read as a command");

    printf("source_gpu_service_start: %u checks passed\n", checks);
    return 0;
}
