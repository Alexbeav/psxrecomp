#ifndef SOURCE_GPU_TEXTURE_H
#define SOURCE_GPU_TEXTURE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t uv[3], window;
    uint16_t page, clut;
    int raw, load_clut;
} SourceGPUTexture;

typedef struct {
    const uint32_t *words;
    uint32_t draw_mode, texture_window;
    int x, y, interlace, clip_left, clip_top, clip_right, clip_bottom;
    unsigned skip_field;
} SourceGPUBlock;

static inline int source_gpu_sprite_opcode(unsigned command)
{
    return command >= 0x60u && command <= 0x7fu;
}
static inline unsigned source_gpu_sprite_class(unsigned command)
{
    return (command >> 3) & 3u;
}
static inline void source_gpu_sprite_extent(unsigned command, const uint32_t *words,
                                            unsigned *width, unsigned *height)
{
    unsigned size = source_gpu_sprite_class(command);
    if (size) {
        *width = *height = size == 1 ? 1u : size == 2 ? 8u : 16u;
    } else {
        uint32_t dimensions = words[(command & 4u) ? 3 : 2];
        *width = dimensions & 1023u;
        *height = (dimensions >> 16) & 511u;
    }
}
#ifdef __cplusplus
}
#endif
#endif
