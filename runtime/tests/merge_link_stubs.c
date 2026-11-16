/* Link stubs for test targets that link dma.c, gpu.c or sio.c without the
 * units that define these symbols (memory.c, psx_cycles.c, func_override.c,
 * interrupts.c). Weak, so a target that links the real unit uses it. */
#include <stdint.h>
#include <stddef.h>

__attribute__((weak)) int g_psx_render_pass_active = 0;
__attribute__((weak)) int psx_mod_host_output_scope(void) { return 0; }
__attribute__((weak)) int psx_mod_gpu_dma_memory_contains(uint32_t address, uint32_t bytes)
{ (void)address; (void)bytes; return 0; }
__attribute__((weak)) const uint8_t *psx_mod_gpu_dma_read_view(uint32_t address, uint32_t bytes)
{ (void)address; (void)bytes; return NULL; }
__attribute__((weak)) int func_override_active(void) { return 0; }
/* sio.c's input-update replay hook lives in debug_server.c: pass buttons through. */
__attribute__((weak)) uint16_t debug_server_update_poll(int slot, uint16_t buttons, int analog)
{ (void)slot; (void)analog; return buttons; }
__attribute__((weak)) unsigned psx_snapshot_host_call_depth(void) { return 0; }
/* The source GPU profile is off unless a target links source_gpu_runtime.c. */
__attribute__((weak)) int source_gpu_runtime_active(void) { return 0; }
__attribute__((weak)) uint64_t psx_next_service_cycle = 0;
__attribute__((weak)) uint64_t g_psx_cycle_fast_limit = 0;
