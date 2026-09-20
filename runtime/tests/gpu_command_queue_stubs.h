#ifndef PSX_TEST_GPU_COMMAND_QUEUE_STUBS_H
#define PSX_TEST_GPU_COMMAND_QUEUE_STUBS_H
#include <stdint.h>
/* These fixtures isolate other devices. No renderer work is pending. */
int gpu_command_queue_dma_ready(void) { return 1; }
int gpu_command_queue_accept_word(void) { return 1; }
#ifndef GPU_COMMAND_QUEUE_EVENT_STUBS_CUSTOM
void gpu_command_queue_advance(void) {}
uint32_t gpu_command_queue_cycles_to_event(void) { return UINT32_MAX; }
#endif
#endif
