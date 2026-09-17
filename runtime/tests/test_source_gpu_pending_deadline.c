/* A pending frontend return must prevent another compiled block from running. */
#define _POSIX_C_SOURCE 200809L /* setenv under -std=c11 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "cpu_state.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "source_gpu_runtime.h"
#pragma GCC diagnostic pop
uint64_t psx_cycle_count, g_psx_cycle_fast_limit, psx_next_service_cycle;
void (*g_psx_cpu_step_boundary_callback)(CPUState *, uint32_t, uint64_t);
uint64_t psx_get_cycle_count(void) {return psx_cycle_count;}
void dma_source_gpu_service_at(uint64_t cycle) {assert(cycle == psx_cycle_count);}
int main(void) {
#ifdef _WIN32
    _putenv_s("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad");
#else
    setenv("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad", 1);
#endif
    source_gpu_runtime_init();
    SourceGPUServiceClock clock;
    SourceGPUCommandProjection command;
    do {
        uint32_t distance=source_gpu_runtime_cycles_to_event();
        assert(distance > 0 && distance <= 128u);
        psx_cycle_count += distance;
        assert(psx_cycle_count < 2000000u);
        source_gpu_runtime_advance();
        source_gpu_runtime_copy(&clock,&command);
    } while(!clock.frame_pending);
    assert(clock.frame_returns == 0 && source_gpu_runtime_cycles_to_event() == 0);
    return 0;
}
