/* A deferred GP0 fill must reach the renderer when time releases its budget.
 * Device seams are explicit counters; unrelated link stubs are never called. */
#include <assert.h>
#include "cpu_state.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "source_gpu_runtime.h"
#pragma GCC diagnostic pop
#include "pst_wire.h"

uint64_t psx_cycle_count, psx_next_service_cycle, g_psx_cycle_fast_limit;
void (*g_psx_cpu_step_boundary_callback)(CPUState *,uint32_t,uint64_t);
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
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
int main(void) {
    uint8_t wire[240], rasters[160];
    source_gpu_runtime_init();
    source_gpu_runtime_set_dispatch_sink(draw);
    source_gpu_service_wire_write(wire);
    PstW w; pst_w_init(&w,wire+44,4); pst_w_i32(&w,-20);
    assert(source_gpu_service_wire_read(wire,sizeof wire));
    source_gpu_runtime_gp0(0x020000ffu);
    source_gpu_runtime_gp0(0);
    source_gpu_runtime_gp0(0x00010010u);
    assert(draws == 0u);
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
    return 0;
}
