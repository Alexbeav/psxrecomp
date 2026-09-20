#ifndef PSX_GPU_COMMAND_QUEUE_H
#define PSX_GPU_COMMAND_QUEUE_H
#include "source_gpu_command_projection.h"

/* Adapt completed queue phases to the existing renderer packet format. */
static inline unsigned gpu_command_queue_packet(const SourceGPUCommandDispatch *d,uint32_t words[12]) {
    unsigned count=d->count;
    memcpy(words,d->words,count*sizeof(uint32_t));
    unsigned op=words[0]>>24;
    if(d->kind==SOURCE_GPU_DISPATCH_QUAD_FIRST || d->kind==SOURCE_GPU_DISPATCH_QUAD_SECOND) {
        unsigned stride=source_gpu_polygon_stride(op);
        count=source_gpu_command_length(words[0]);
        if(d->kind==SOURCE_GPU_DISPATCH_QUAD_SECOND) {
            for(unsigned i=0;i<3;i++) {
                unsigned v=i+1;
                words[1+stride*i]=d->words[1+stride*v];
                if(op&4)words[2+stride*i]=d->words[2+stride*v];
                if((op&0x10) && i)words[stride*i]=d->words[stride*v];
            }
            if(op&0x10)words[0]=(words[0]&0xff000000u)|(d->words[stride]&0xffffffu);
            if(op&4) {
                words[2]=(words[2]&0xffffu)|(d->words[2]&0xffff0000u);
                words[2+stride]=(words[2+stride]&0xffffu)|(d->words[2+stride]&0xffff0000u);
            }
        }
        words[0]&=~0x08000000u;
    } else if(d->kind==SOURCE_GPU_DISPATCH_COMMAND && source_gpu_line_polyline(op)) {
        /* The queue emits complete two-point segments, not an open strip. */
        words[0]&=~0x08000000u;
    }
    return count;
}

/* No event is due for an incomplete packet or a pending CPU readback. */
static inline uint32_t gpu_command_queue_deadline(const SourceGPUCommandProjection *s) {
    if(!s->count || s->phase==8)return UINT32_MAX;
    unsigned need=source_gpu_command_length(s->queue[0]);
    int debt_applies=1;
    if (s->phase==2) need=source_gpu_polygon_stride(s->command);
    else if (s->phase==4) { need=1; debt_applies=0; }
    else if (s->pline) {
        need=source_gpu_line_terminator(s->queue[0]) ? 1 : source_gpu_line_segment_length(s->pline_command);
    } else {
        unsigned op=s->queue[0]>>24;
        if (op==0 || (op>=0xe3 && op<=0xe5)) debt_applies=0;
    }
    if (s->count<need) return UINT32_MAX;
    if (debt_applies && s->budget<0) return (uint32_t)((-(int64_t)s->budget+1)/2);
    return 1;
}
#endif
