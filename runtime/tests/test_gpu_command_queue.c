/* Authored ordinary-renderer queue checks. Reuse renderer seams only;
 * production gpu.c owns parsing, status, timing and snapshots. No guest ROM. */
/* This test supplies its own live clock, so the fixture must not define one. */
#define GPU_QUEUE_HOST_CLOCK 1
#define main textured_dot_fixture_main
#include "test_gpu_textured_dot_nw_shift_exec.c"
#undef main
#ifndef GPU_QUEUE_REAL_CLOCK
uint64_t psx_cycle_count, psx_next_service_cycle, g_psx_cycle_fast_limit;
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
#endif
#ifndef TEST_BASE
static void tick(unsigned n) { psx_cycle_count += n; gpu_queue_service(); }
#else
static void tick(unsigned n) { psx_cycle_count += n; }
#endif
static void fill(void) {
    gpu_write_gp0(0x020000ffu); gpu_write_gp0(0); gpu_write_gp0(0x00f00140u);
}
/* Busy time charged for one gouraud triangle spanning rows 0..199 of a
 * 640x480 drawing area, with GP0(E1h) bit 10 as given. */
static int shaded_charge(unsigned interlaced_480, unsigned draw_to_display_bit) {
    reset_gpu_state_for_test(); gp1_reset_command_buffer();
    vertical_interlace=vres=interlaced_480;
    display_area_y=0; lcf=1;
    gpu_write_gp0(0xe3000000u); gpu_write_gp0(0xe4000000u|(479u<<10)|639u);
    gpu_write_gp0(0xe5000000u); gpu_write_gp0(0xe1000000u|(draw_to_display_bit<<10));
    tick(20000);
    int before=gpu_queue.credit;
    gpu_write_gp0(0x30ff0000u); gpu_write_gp0(0x00000000u);
    gpu_write_gp0(0x0000ff00u); gpu_write_gp0(0x0000027fu);
    gpu_write_gp0(0x000000ffu); gpu_write_gp0(0x00c80140u);
    return before-gpu_queue.credit;
}
#ifndef GPU_QUEUE_NO_MAIN
int main(int argc, char **argv) {
    reset_gpu_state_for_test();
    const char *mode=argc>1?argv[1]:"pending";
    if (!strcmp(mode,"pending")) {
        gpu_write_gp0(0x280000ffu);
        assert(!(gpu_read_gpustat() & (1u << 26)));
        assert(!(gpu_read_gpustat() & (1u << 28)));
    } else if (!strcmp(mode,"deferred")) {
        fill(); gpu_write_gp0(0xe1000123u);
        assert(texpage_x == 0);
        for(unsigned i=0;i<200;i++) (void)gpu_read_gpustat();
        assert(texpage_x == 0 && psx_cycle_count == 0);
        tick(20000); assert(texpage_x == 3);
    } else if (!strcmp(mode,"reset")) {
        fill(); gpu_write_gp0(0xe1000123u);
        assert(texpage_x == 0);
        gp1_reset_command_buffer(); tick(20000); assert(texpage_x == 0);
    } else if (!strcmp(mode,"snapshot")) {
        fill(); gpu_write_gp0(0xe1000123u);
        assert(texpage_x == 0);
        uint32_t size=gpu_snapshot_bytes(); uint8_t *wire=malloc(size);
        gpu_snapshot_write(wire);
        uint8_t saved=wire[size-208]; wire[size-208]=17;
        assert(!gpu_snapshot_read(wire,size) && texpage_x==0);
        wire[size-208]=saved;
        assert(!gpu_snapshot_read(wire,size-1));
        tick(20000); assert(texpage_x == 3);
        psx_cycle_count=0; assert(gpu_snapshot_read(wire,size));
        assert(texpage_x == 0); tick(20000); assert(texpage_x == 3); free(wire);
    } else if (!strcmp(mode,"packets")) {
        fill(); unsigned draws=gp0_draw_count;
        gpu_set_gp0_source(0x1004); gpu_ws_restore_linked_list_rank(7);
        gpu_write_gp0(0x280000ff); gpu_write_gp0(0x000a000a);
        gpu_write_gp0(0x000a0014); gpu_write_gp0(0x0014000a);
        assert(gp0_draw_count==draws);
        tick(20000); assert(gp0_draw_count==draws);
        gpu_write_gp0(0x00140014); assert(gp0_draw_count==draws+1);
        assert(gp0_cmd_source_addr==0x1004);
        /* A polyline and its terminator precede the next attribute. */
        gpu_write_gp0(0x480000ff); gpu_write_gp0(0x000a000a);
        gpu_write_gp0(0x000a0014); gpu_write_gp0(0x00140014);
        gpu_write_gp0(0x50005000); gpu_write_gp0(0xe1000123);
        assert(texpage_x==0); tick(20000);
        assert(texpage_x==3 && gp0_state==GP0_IDLE);
    } else if (!strcmp(mode,"interlace-charge")) {
        /* PSX-SPX GPUSTAT.10: in 480-line interlace with drawing to the
         * displayed area prohibited, the GPU skips the displayed field's
         * lines. Busy time follows the rows actually drawn. */
        int full=shaded_charge(0,0), allowed=shaded_charge(1,1), skipped=shaded_charge(1,0);
        int setup=SOURCE_GPU_T_POLYGON_SETUP(0x30u,0);
        assert(full>setup && allowed==full);
        assert(skipped>setup && skipped<full);
        int half=(full-setup)/2, rows=skipped-setup;
        assert(rows>=half-(full-setup)/100 && rows<=half+(full-setup)/100);
    } else abort();
    printf("PASS %s\n",mode); return 0;
}

#endif
