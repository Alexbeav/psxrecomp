#ifndef PSX_SOURCE_GPU_TEXTURE_H
#define PSX_SOURCE_GPU_TEXTURE_H
#include <stdint.h>

/* Sprite/rectangle opcode geometry, shared by the command projection (which
 * charges the raster cost) and the software renderer (which draws it), so the
 * two can never disagree about a packet's shape. As the source core's
 * SPR_HELPER reads it: bit 2 selects a textured packet and bits 3-4 select the
 * size class, where class 0 carries an explicit width/height word and classes
 * 1/2/3 are the fixed 1x1, 8x8 and 16x16 forms that carry no size word. */
static inline int source_gpu_sprite_opcode(unsigned command) {
    return command>=0x60 && command<=0x7f;
}
static inline unsigned source_gpu_sprite_class(unsigned command) { return (command>>3)&3u; }
/* Width and height of a sprite packet: from the size word for class 0, from
 * the opcode otherwise. `words` is the packet base. */
static inline void source_gpu_sprite_extent(unsigned command,const uint32_t *words,
                                            unsigned *width,unsigned *height) {
    unsigned klass=source_gpu_sprite_class(command);
    if(klass){*width=*height=klass==1?1u:klass==2?8u:16u;return;}
    unsigned size=words[(command&4u)?3:2];
    *width=size&1023u;*height=(size>>16)&511u;
}

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
