/* Fixtures without a GPU renderer keep its ordinary FIFO empty. */
#ifndef GPU_QUEUE_TEST_STUBS_H
#define GPU_QUEUE_TEST_STUBS_H
#include <stdint.h>
int gpu_queue_has_space(void) { return 1; }
void gpu_queue_service(void) {}
uint32_t gpu_queue_cycles_to_event(void) { return UINT32_MAX; }
#endif
