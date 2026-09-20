#ifndef PSX_GPU_COMMAND_QUEUE_WIRE_H
#define PSX_GPU_COMMAND_QUEUE_WIRE_H
#include "source_gpu_command_projection.h"
#include "pst_wire.h"

/* Field encoding, never host padding. The live queue must travel with VRAM
 * and DMA progress or a restore can drop commands and rewind its clock alone. */
#define GPU_COMMAND_QUEUE_WIRE_BYTES 340u
#define GPU_QUEUE_SCALARS(U,I) \
    U(count) U(phase) U(command) \
    I(clip_x0) I(clip_y0) I(clip_x1) I(clip_y1) I(offset_x) I(offset_y) \
    U(draw_mode) U(texture_window) U(mask_bits) U(display_mode) U(dma_direction) \
    U(field_valid) U(skip_field) U(first_triangles) U(second_triangles) \
    U(pline) U(pline_command) U(pline_color) U(pline_vertex) \
    U(transfer_words) U(dispatch.kind) U(dispatch.count) I(error)

static inline int gpu_command_queue_write(PstW *w,const SourceGPUCommandProjection *s) {
    if(!pst_w_i32(w,s->budget) || !pst_w_u64(w,s->last_update))return 0;
#define U(f) if(!pst_w_u32(w,s->f))return 0;
#define I(f) if(!pst_w_i32(w,s->f))return 0;
    GPU_QUEUE_SCALARS(U,I)
#undef U
#undef I
    for(unsigned i=0;i<32;i++)if(!pst_w_u32(w,s->queue[i]))return 0;
    for(unsigned i=0;i<12;i++)if(!pst_w_u32(w,s->polygon_words[i]))return 0;
    for(unsigned i=0;i<12;i++)if(!pst_w_u32(w,s->dispatch.words[i]))return 0;
    return 1;
}
static inline int gpu_command_queue_read(PstR *r,SourceGPUCommandProjection *out) {
    SourceGPUCommandProjection s={0};
    if(!pst_r_i32(r,&s.budget) || !pst_r_u64(r,&s.last_update))return 0;
#define U(f) if(!pst_r_u32(r,&s.f))return 0;
#define I(f) if(!pst_r_i32(r,&s.f))return 0;
    GPU_QUEUE_SCALARS(U,I)
#undef U
#undef I
    for(unsigned i=0;i<32;i++)if(!pst_r_u32(r,&s.queue[i]))return 0;
    for(unsigned i=0;i<12;i++)if(!pst_r_u32(r,&s.polygon_words[i]))return 0;
    for(unsigned i=0;i<12;i++)if(!pst_r_u32(r,&s.dispatch.words[i]))return 0;
    if(s.count>32 || s.dispatch.count>12 || s.dispatch.kind>SOURCE_GPU_DISPATCH_UPLOAD_WORD ||
       !(s.phase==0 || s.phase==2 || s.phase==4 || s.phase==8) ||
       s.field_valid>1 || s.skip_field>1 || s.pline>1 || s.error)return 0;
    if((s.phase==4 || s.phase==8) && !s.transfer_words)return 0;
    if(s.phase==2 && (!source_gpu_polygon_supported(s.command) || !(s.command&8u) ||
       (s.polygon_words[0]>>24)!=s.command))return 0;
    if(s.pline && !source_gpu_line_polyline(s.pline_command))return 0;
    *out=s;return 1;
}
#undef GPU_QUEUE_SCALARS
#endif
