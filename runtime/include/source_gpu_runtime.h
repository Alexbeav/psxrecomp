#ifndef PSX_SOURCE_GPU_RUNTIME_H
#define PSX_SOURCE_GPU_RUNTIME_H
#include "source_gpu_service_clock.h"
#include "source_gpu_command_projection.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental source-profile service bridge. Initialization is cold-only;
 * default code remains inactive. Reads copy scalar state without servicing it.
 * The GPU installs a sink so phase dispatch owns renderer/environment effects. */
typedef int (*SourceGPUDispatchSink)(const SourceGPUCommandDispatch *);
void source_gpu_runtime_set_dispatch_sink(SourceGPUDispatchSink);
void source_gpu_runtime_init(void);
int source_gpu_runtime_active(void);
/* TAS checkpoint resume: set the frontend-return counter to match a restored
 * state so probes and the input route continue from the saved return. */
int source_gpu_runtime_set_frame_returns(uint32_t frame);
int source_gpu_runtime_ready(void);
uint32_t source_gpu_runtime_status_bits(void);
void source_gpu_runtime_advance(void);
uint32_t source_gpu_runtime_cycles_to_event(void);
void source_gpu_runtime_dma_write(void);
void source_gpu_runtime_gp0(uint32_t word);
void source_gpu_runtime_read(void);
void source_gpu_runtime_gp1(uint32_t word);
void source_gpu_runtime_copy(SourceGPUServiceClock *,SourceGPUCommandProjection *);
void source_gpu_runtime_copy_return(SourceGPUServiceClock *,SourceGPUCommandProjection *);
#ifdef __cplusplus
}
#endif
#endif
