/* Exercise the real producer and presenter with synthetic, source-owned pixels.
 * Link with function/data sections and --gc-sections: unused GPU services drop.
 * This qualifies presentation policy, not PS1 scanout hardware fidelity. */
#ifndef GPU_IMPLEMENTATION
#define GPU_IMPLEMENTATION "../src/gpu.c"
#endif
#include GPU_IMPLEMENTATION
#include <assert.h>

static int test_streaming;
int mdec_recently_active(uint32_t within_frames) {
    assert(within_frames == 10);
    return test_streaming;
}
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t* p) {
    (void)x; (void)y; (void)w; (void)h; (void)p;
}
void text_xlate_vram_upload(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}

int main(void) {
    GpuDisplayInfo di = {0};
    uint32_t out[4];
    /* Packed bytes repeat 12 12 12, so decoded gray is unambiguous. */
    for (unsigned i = 0; i < 1024u * 512u; ++i) vram[i] = 0x1212;
    gp1_display_mode(0x10);
    gpu_depth24_present_row(&di, 20, out, 4);
    assert(out[0] == 0xff121212); /* preuploaded still remains visible */
    test_streaming = 1;
    gpu_depth24_present_row(&di, 0, out, 4);
    gpu_depth24_present_row(&di, 20, out, 4);
    assert(out[0] == 0xff000000); /* unwritten movie rows are hidden */
    vram_write_x = 0; vram_write_y = 20; vram_write_w = 6; vram_write_h = 1;
    gp0_commit_cpu_to_vram();
    gpu_depth24_present_row(&di, 20, out, 4);
    assert(out[3] == 0xff121212); /* committed movie pixels retain RGB bytes */
    gpu_depth24_present_row(&di, 21, out, 4);
    assert(out[0] == 0xff000000); /* neighbor is still unwritten */
    test_streaming = 0;
    gpu_depth24_present_row(&di, 0, out, 4);
    test_streaming = 1;
    gpu_depth24_present_row(&di, 0, out, 4);
    gpu_depth24_present_row(&di, 20, out, 4);
    assert(out[0] == 0xff000000); /* still-to-movie boundary clears coverage */
    gp0_commit_cpu_to_vram();
    gpu_depth24_present_row(&di, 0, out, 4);
    gpu_depth24_present_row(&di, 20, out, 4);
    assert(out[0] == 0xff121212); /* steady movie preserves the new upload */
    vram_write_y = 511; vram_write_h = 2;
    gp0_commit_cpu_to_vram();
    gpu_depth24_present_row(&di, 0, out, 4);
    assert(out[0] == 0xff121212); /* rectangle wraps at physical row 512 */
    gp1_display_mode(0);
    gp1_display_mode(0x10);
    gpu_depth24_present_row(&di, 20, out, 4);
    assert(out[0] == 0xff000000); /* new depth session drops old coverage */
    assert(vram[20u * 1024u] == 0x1212); /* policy never alters guest VRAM */
    return 0;
}
