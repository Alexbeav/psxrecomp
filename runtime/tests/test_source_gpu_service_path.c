/* A deferred GP0 fill must reach the renderer when time releases its budget.
 * Device seams are explicit counters; unrelated link stubs are never called. */
#include <assert.h>
#include <stdlib.h>
#include "source_fixture_assert.h"
#include "cpu_state.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "source_gpu_runtime.h"
#pragma GCC diagnostic pop
#include "pst_wire.h"
#include "psx_cycles.h"
#include "psx_cycle_freeze.h"

uint64_t psx_cycle_count, psx_next_service_cycle, g_psx_cycle_fast_limit;
uint64_t g_psx_device_gen;
void (*g_psx_cpu_step_boundary_callback)(CPUState *,uint32_t,uint64_t);
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void psx_devices_service_to_now(void) { source_gpu_runtime_advance(); }
static unsigned dma_calls, draws;
static uint64_t last_dma;
void dma_source_gpu_service_at(uint64_t cycle) {
    assert(cycle >= last_dma && cycle <= psx_cycle_count);
    last_dma = cycle;
    ++dma_calls;
}
static int draw(const SourceGPUCommandDispatch *command) {
    assert(command->count == 3u && command->words[0] == 0x020000ffu);
    ++draws;
    return 0;
}
static uint32_t terminal_frame;
uint32_t input_route_observer_terminal_frame(void) { return terminal_frame; }
int dma_cpu_source_halted(void) { return 0; }
int main(int argc, char **argv) {
    if (argc == 2) {
        CPUState cpu = {0};
        source_gpu_runtime_init();
        assert(source_gpu_runtime_set_frame_returns(7));
        terminal_frame = (uint32_t)strtoul(argv[1], 0, 10);
#ifdef PSX_SOURCE_BUS_BOUNDARY
        source_gpu_runtime_cpu_boundary(&cpu,0x80010000u);
#else
        g_psx_cpu_step_boundary_callback(&cpu, 0x80010000u, 0);
#endif
        assert(!terminal_frame); /* An active terminal must either finish or reject. */
        return 0;
    }
    uint8_t wire[SOURCE_GPU_SERVICE_WIRE_BYTES], empty[SOURCE_GPU_SERVICE_WIRE_BYTES], rasters[160];
    CPUState inactive={0},unchanged;inactive.gpr[1]=123;unchanged=inactive;
    g_psx_guest_time_frozen=1;g_psx_cyc_batch=4;
    source_gpu_runtime_cpu_boundary(&inactive,0);
    assert(!memcmp(&inactive,&unchanged,sizeof inactive) && g_psx_cyc_batch==4 && psx_cycle_count==0);
    g_psx_guest_time_frozen=0;g_psx_cyc_batch=0;
    source_gpu_runtime_init();
    source_gpu_runtime_set_dispatch_sink(draw);
    source_gpu_service_wire_write(wire);
    PstW w; pst_w_init(&w,wire+44,4); pst_w_i32(&w,-20);
    assert(source_gpu_service_wire_read(wire,sizeof wire));
    source_gpu_service_wire_write(empty);
    source_gpu_runtime_gp0(0x020000ffu);
    source_gpu_runtime_gp0(0);
    source_gpu_runtime_gp0(0x00010010u);
    assert(draws == 0u);
    assert(!source_gpu_service_queue_empty());
    source_gpu_service_wire_write(wire);
    assert(source_gpu_service_wire_read(empty,sizeof empty));
    assert(source_gpu_service_queue_empty());
    assert(source_gpu_service_wire_read(wire,sizeof wire));
    assert(!source_gpu_service_queue_empty());
    psx_cycle_count=128u;
    source_gpu_runtime_advance();
    assert(draws == 1u && dma_calls == 1u);
    source_gpu_raster_wire_write(rasters);
    /* Each raster starts with its absolute cycle. Both must advance. */
    assert(rasters[0] == 128u && rasters[80] == 128u);
    /* A frontend return can be due before the next scheduled GPU/DMA event.
     * A compiled block must not pass that boundary without a precise owner. */
    SourceGPUServiceClock clock;
    SourceGPUCommandProjection command;
    do {
        uint32_t distance = source_gpu_runtime_cycles_to_event();
        assert(distance > 0 && distance <= 128u);
        psx_cycle_count += distance;
        assert(psx_cycle_count < 2000000u);
        source_gpu_runtime_advance();
        source_gpu_runtime_copy(&clock, &command);
    } while (!clock.frame_pending);
    assert(clock.frame_returns == 0 && source_gpu_runtime_cycles_to_event() == 0);
    /* Configured complete context: malformed fields must refuse atomically. */
    uint8_t complete[552], bad[552], after[552];
    source_gpu_runtime_snapshot_write(complete);
    assert(source_gpu_runtime_snapshot_read(complete,sizeof complete,0));
    const unsigned offsets[]={0,4,8,16,24,40,44,56,60,64,76,80,84,88,92,96,100,104,108,112,116,120,124,136,140,200,204,208,260,412,416,420,424,428,432,436,440,444,448,456,460,464,468,492,496,500,504,508,512,516,520,524,528,536,540,544,548};
    unsigned rejected=0;
    for(unsigned i=0;i<sizeof offsets/sizeof *offsets;++i){
        memcpy(bad,complete,sizeof bad);memset(bad+offsets[i],0xff,4);bad[offsets[i]+3]=0x7f;
        assert(!source_gpu_runtime_snapshot_read(bad,sizeof bad,1));
        source_gpu_runtime_snapshot_write(after);assert(!memcmp(complete,after,sizeof after));++rejected;
    }
    assert(!source_gpu_runtime_snapshot_read(complete,sizeof complete-1,1));
    assert(source_gpu_runtime_snapshot_read(complete,sizeof complete,1));
    printf("source GPU context: %u malformed/config controls reject atomically\n",rejected);
    CPUState cpu = {0};
    terminal_frame = 1; /* Terminal declared before its pending source return. */
#ifdef PSX_SOURCE_BUS_BOUNDARY
    source_gpu_runtime_cpu_boundary(&cpu,0x80010000u);
#else
    g_psx_cpu_step_boundary_callback(&cpu, 0x80010000u, psx_cycle_count);
#endif
    assert(0);
    return 1;
}

uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
uint32_t g_psx_gcs_recip_q16=65536u,g_psx_gcs_frac;
int psx_in_device_service,g_ls_replay_active,g_event_step_conservative;

uint32_t *g_psx_cyc_local_acc;

int g_psx_guest_time_frozen,g_psx_render_pass_active;
