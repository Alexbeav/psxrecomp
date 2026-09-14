#ifndef PSX_SOURCE_GPU_TEXTURE_H
#define PSX_SOURCE_GPU_TEXTURE_H
#include <stdint.h>
/* Parameters for the opt-in, native software source-comparison renderer.
 * These are command attributes, never imported source-emulator state. */
typedef struct SourceGPUTexture {
    uint32_t uv[3],window;
    uint16_t page,clut;
    int raw,load_clut;
} SourceGPUTexture;
typedef struct SourceGPUBlock {
    const uint32_t *words;
    uint32_t draw_mode,texture_window;
    int x,y,interlace,clip_left,clip_top,clip_right,clip_bottom;
    unsigned skip_field;
} SourceGPUBlock;
#endif
