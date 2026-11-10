/* gpu_raster_skipped_row() for the source-comparison GPU fixtures.
 *
 * Those fixtures link gpu_sw_renderer.c on its own, without gpu.c, which is
 * where the real definition lives. The wave-4 renderer calls it per primitive
 * to protect the active field's row parity in 480i.
 *
 * The fixtures compare against a progressive reference, so report progressive.
 * Per gpu_interlace.h, -1 means no row parity is protected: the renderer's
 * `((y / s) & 1) == skipped_row` guard is then never true and nothing is
 * skipped, which is the behaviour these comparisons were qualified against.
 */
#include "gpu_interlace.h"

int gpu_raster_skipped_row(void) { return -1; }
