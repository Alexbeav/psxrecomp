#ifndef PSX_SOURCE_GPU_COMMAND_PROJECTION_H
#define PSX_SOURCE_GPU_COMMAND_PROJECTION_H
#include <stdint.h>
#include <string.h>
#include "source_gpu_polygon_projection.h"

/* Experimental, independently expressed Octoshock 2.2.2 command timing.
 * This is NOT a renderer or a hardware timing claim. The synchronous renderer
 * returns native texture-cache work, charged to this same queue before its
 * next phase. Cache data and tags remain owned by the native renderer. Call update only at an
 * explicitly qualified source GPU service event; reads do not advance it.
 * Current scope: NOP/cache-clear, drawing environment, A0/C0 transfers,02 fills,80 copies, variable rectangles,
 * observed untextured/textured polygons, including general opaque flat quads,
 * with inclusive clipping. Vertex coordinates after offsets must stay in the
 * signed 11-bit range; clipping stays inside VRAM.
 * Interlaced row skipping requires explicitly supplied live readout parity.
 * Other command families and state restoration remain unsupported. Ordinary GP1 reset
 * preserves nonnegative credit and clamps debt, rather than granting new work.
 * Original source: gpu.cpp ProcessFIFO/Update; gpu_polygon.cpp draw costs.
 */
enum { SOURCE_GPU_DISPATCH_NONE, SOURCE_GPU_DISPATCH_COMMAND,
       SOURCE_GPU_DISPATCH_QUAD_FIRST, SOURCE_GPU_DISPATCH_QUAD_SECOND,
       SOURCE_GPU_DISPATCH_UPLOAD_WORD };
typedef struct SourceGPUCommandDispatch {
    unsigned kind, count;
    uint32_t words[12];
} SourceGPUCommandDispatch;
typedef struct SourceGPUCommandProjection {
    int32_t budget;
    uint32_t queue[32], count, phase, command;
    uint64_t last_update;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int offset_x,offset_y;
    uint32_t draw_mode,texture_window,mask_bits,display_mode,dma_direction;
    unsigned field_valid,skip_field;
    unsigned first_triangles, second_triangles;
    uint32_t polygon_words[12];
    uint32_t transfer_words;
    SourceGPUCommandDispatch dispatch;
    int error;
} SourceGPUCommandProjection;

enum { SOURCE_GPU_COMMAND_UNSUPPORTED=1, SOURCE_GPU_COMMAND_OVERFLOW=2,
       SOURCE_GPU_COMMAND_REVERSE_TIME=3 };

static inline void source_gpu_command_cold(SourceGPUCommandProjection *s) {
    memset(s,0,sizeof(*s));
}

static inline unsigned source_gpu_polygon_stride(unsigned opcode) {
    return 1u+!!(opcode&0x10)+!!(opcode&4);
}
static inline int source_gpu_polygon_supported(unsigned opcode) {
    switch(opcode) {
    case 0x20:case 0x21:case 0x22:case 0x23:
    case 0x28:case 0x29:case 0x2a:case 0x2b:case 0x30:case 0x38:case 0x3a:
    case 0x24:case 0x2c:case 0x2d:case 0x2e:case 0x2f:
    case 0x34:case 0x3c:case 0x3e:return 1;
    default:return 0;
    }
}
static inline unsigned source_gpu_polygon_setup(unsigned opcode) {
    return opcode&4 ? (opcode&0x10 ? 450:180) : (opcode&0x10 ? 288:0);
}
static inline int source_gpu_block_supported(unsigned command) {
    return command==2 || command==0x80 || command==0x60 || command==0x62 || command==0x64 || command==0x65 || command==0x66;
}
static inline unsigned source_gpu_command_length(uint32_t word) {
    unsigned command=word>>24;
    if(source_gpu_block_supported(command))return command==0x80 || command==0x64 || command==0x65 || command==0x66?4:3;
    if(source_gpu_polygon_supported(command))return 1+3*source_gpu_polygon_stride(command)-!!(command&0x10);
    return command==0xa0 || command==0xc0 ? 3u : 1u;
}
static inline unsigned source_gpu_command_feedback_length(uint32_t word) {
    unsigned command=word>>24;
    if(command==2 || (command>=0x60 && command<=0x66))return 3;
    return command==0x80 || command==1 || command==0xa0 || command==0xc0 || command==0xe1 || command==0xe2 || command==0xe6 ? 2u : 1u;
}

static inline int source_gpu_command_ready(const SourceGPUCommandProjection *s) {
    if(s->error) return -1;
    if(s->phase==2) return 0;
    if(s->count && (s->phase==4 || s->phase==8))return 0;
    /* Feedback length differs from packet length. Cache clear and ordinary
     * environment commands admit one queued word while DMA-ready stays set. */
    return s->count==0 || s->count<source_gpu_command_feedback_length(s->queue[0]);
}

static inline uint32_t source_gpu_command_pop(SourceGPUCommandProjection *s) {
    uint32_t word=s->queue[0];
    --s->count;
    memmove(s->queue,s->queue+1,s->count*sizeof(s->queue[0]));
    return word;
}

static inline int source_gpu_command_coord(uint32_t word,unsigned shift) {
    unsigned v=(word>>shift)&2047u;
    return (int)(v^1024u)-1024;
}

static inline int source_gpu_command_polygon_cost(const SourceGPUCommandProjection *s,const uint32_t *words,int second) {
    unsigned opcode=words[0]>>24,stride=source_gpu_polygon_stride(opcode);
    int x[3],y[3];
    if(s->clip_y0>511 || s->clip_y1>511 ||
       ((s->display_mode&0x24)==0x24 && !(s->draw_mode&0x400) && !s->field_valid))return -1;
    for(unsigned i=0;i<3;i++) {
        unsigned v=i+(second?1:0);
        x[i]=source_gpu_command_coord(words[1+stride*v],0)+s->offset_x;
        y[i]=source_gpu_command_coord(words[1+stride*v],16)+s->offset_y;
    }
    return source_poly_cost(x,y,s->clip_x0,s->clip_y0,s->clip_x1,s->clip_y1,
        !!(opcode&0x14),!!((opcode&2)||(s->mask_bits&2)),
        (s->display_mode&0x24)==0x24 && !(s->draw_mode&0x400),s->skip_field);
}

/* Sprite coordinates wrap after adding the drawing offset. Polygon vertex
 * admission and fill/copy addressing have different rules. */
static inline int source_gpu_sprite_origin(uint32_t word,unsigned shift,int offset) {
    return source_gpu_command_coord((uint32_t)(source_gpu_command_coord(word,shift)+offset),0);
}
static inline int source_gpu_command_block_cost(const SourceGPUCommandProjection *s,const uint32_t *words) {
    unsigned opcode=words[0]>>24;
    if(opcode==0x80) {
        unsigned w=words[3]&1023u,h=(words[3]>>16)&511u;
        return 2*(w?w:1024)*(h?h:512);
    }
    int interlace=(s->display_mode&0x24)==0x24 && !(s->draw_mode&0x400);
    if(interlace && !s->field_valid)return -1;
    int cost=opcode==2?46:16;
    if(opcode==2) {
        unsigned w=((words[2]&1023u)+15u)&~15u,h=(words[2]>>16)&511u;
        for(unsigned y=0;y<h;y++) {
            unsigned row=(y+(words[1]>>16))&511u;
            if(!interlace || (row&1u)!=s->skip_field)cost+=(w>>3)+9;
        }
        return cost;
    }
    int x=source_gpu_sprite_origin(words[1],0,s->offset_x),y=source_gpu_sprite_origin(words[1],16,s->offset_y);
    unsigned size=words[(opcode&4)?3:2];
    int right=x+(int)(size&1023u),bottom=y+(int)((size>>16)&511u);
    if(x<s->clip_x0)x=s->clip_x0;if(y<s->clip_y0)y=s->clip_y0;
    if(right>s->clip_x1+1)right=s->clip_x1+1;if(bottom>s->clip_y1+1)bottom=s->clip_y1+1;
    int width=right-x;
    if(width>0)for(int row=y;row<bottom;row++) {
        if(interlace && ((unsigned)row&1u)==s->skip_field)continue;
        cost+=width;
        if((opcode&2) || (s->mask_bits&2))cost+=(((right+1)&~1)-(x&~1))/2;
    }
    return cost;
}

static inline int source_gpu_command_process(SourceGPUCommandProjection *s) {
    s->dispatch.kind=SOURCE_GPU_DISPATCH_NONE;
    if(s->error) return 0;
    if(!s->count) return 1;
    if(s->phase==8)return 1; /* Queued work waits for ordinary GPUREAD. */
    if(s->phase==4) {
        s->dispatch.kind=SOURCE_GPU_DISPATCH_UPLOAD_WORD;s->dispatch.count=1;
        s->dispatch.words[0]=source_gpu_command_pop(s);
        if(--s->transfer_words==0)s->phase=0;
        return 1; /* Upload data bypasses draw debt and ordinary dispatch cost. */
    }
    if(s->phase==2) {
        if(s->budget<0) return 1;
        unsigned stride=source_gpu_polygon_stride(s->command);
        unsigned first=source_gpu_command_length(s->polygon_words[0]),total=first+stride;
        if(s->count<stride)return 1;
        for(unsigned i=0;i<stride;i++)s->polygon_words[first+i]=s->queue[i];
        int cost=source_gpu_command_polygon_cost(s,s->polygon_words,1);
        if(cost<0){s->error=SOURCE_GPU_COMMAND_UNSUPPORTED;return 0;}
        s->dispatch.kind=SOURCE_GPU_DISPATCH_QUAD_SECOND;s->dispatch.count=total;
        memcpy(s->dispatch.words,s->polygon_words,total*sizeof(uint32_t));
        for(unsigned i=0;i<stride;i++)source_gpu_command_pop(s);
        s->budget-=46+source_gpu_polygon_setup(s->command)+cost;
        s->phase=0;++s->second_triangles;return 1;
    }

    unsigned command=s->queue[0]>>24;
    if(source_gpu_block_supported(command)) {
        unsigned n=source_gpu_command_length(s->queue[0]);
        if(s->budget<0 || s->count<n)return 1;
        int cost=source_gpu_command_block_cost(s,s->queue);
        if(cost<0){s->error=SOURCE_GPU_COMMAND_UNSUPPORTED;return 0;}
        s->dispatch.kind=SOURCE_GPU_DISPATCH_COMMAND;s->dispatch.count=n;
        for(unsigned i=0;i<n;i++)s->dispatch.words[i]=source_gpu_command_pop(s);
        s->budget-=2+cost;return 1;
    }
    if(source_gpu_polygon_supported(command)) {
        unsigned n=source_gpu_command_length(s->queue[0]);
        if(s->budget<0 || s->count<n)return 1;
        int cost=source_gpu_command_polygon_cost(s,s->queue,0);
        if(cost<0){s->error=SOURCE_GPU_COMMAND_UNSUPPORTED;return 0;}
        s->dispatch.kind=(command&8)?SOURCE_GPU_DISPATCH_QUAD_FIRST:SOURCE_GPU_DISPATCH_COMMAND;
        s->dispatch.count=n;
        for(unsigned i=0;i<n;i++)s->polygon_words[i]=s->dispatch.words[i]=source_gpu_command_pop(s);
        s->budget-=84+source_gpu_polygon_setup(command)+cost;
        if(command&4)s->draw_mode=(s->draw_mode&~0x1ffu)|((s->polygon_words[4+!!(command&0x10)]>>16)&0x1ffu);
        if(command&8){s->phase=2;s->command=command;++s->first_triangles;}
        return 1;
    }
    if(command!=0 && command!=1 && !(command>=0xe1 && command<=0xe6) && command!=0xa0 && command!=0xc0) {
        s->error=SOURCE_GPU_COMMAND_UNSUPPORTED; return 0;
    }
    unsigned ordinary_environment=command==0xe1 || command==0xe2 || command==0xe6;
    if(ordinary_environment && s->budget<0)return 1;
    if(command==0xa0 || command==0xc0) {
        if(s->budget<0 || s->count<3)return 1;
        unsigned width=s->queue[2]&1023u;
        unsigned height=(s->queue[2]>>16)&(command==0xa0?511u:1023u);
        if(!width)width=1024;
        if(command==0xa0 && !height)height=512;
        if(command==0xc0 && height>512)height&=511u;
        s->dispatch.kind=SOURCE_GPU_DISPATCH_COMMAND;s->dispatch.count=3;
        for(unsigned i=0;i<3;i++)s->dispatch.words[i]=source_gpu_command_pop(s);
        s->budget-=2;
        s->transfer_words=(width*height+1u)/2u;
        if(s->transfer_words)s->phase=command==0xa0?4u:8u;
    } else if(command==1) {
        if(s->budget<0)return 1;
        s->dispatch.kind=SOURCE_GPU_DISPATCH_COMMAND;s->dispatch.count=1;
        s->dispatch.words[0]=source_gpu_command_pop(s);s->budget-=2;
        /* Cache invalidation is a synchronous native renderer side effect. */
    } else {
        uint32_t word=source_gpu_command_pop(s);
        s->dispatch.kind=SOURCE_GPU_DISPATCH_COMMAND;s->dispatch.count=1;
        s->dispatch.words[0]=word;
        if(ordinary_environment)s->budget-=2;
        /* Original NULLCMD and clip commands are ss_cmd: neither block on
         * negative work credit nor charge the ordinary dispatch cost.
         * Draw mode, texture window and mask commands use ordinary dispatch. */
        if(command==0xe3) { s->clip_x0=word&1023; s->clip_y0=(word>>10)&1023; }
        if(command==0xe4) { s->clip_x1=word&1023; s->clip_y1=(word>>10)&1023; }
        if(command==0xe1) s->draw_mode=word&0x3fffu;
        if(command==0xe2) s->texture_window=word&0xfffffu;
        if(command==0xe5) {s->offset_x=source_gpu_command_coord(word,0);s->offset_y=source_gpu_command_coord(word,11);}
        if(command==0xe6) s->mask_bits=word&3u;
    }
    return 1;
}

static inline int source_gpu_command_gp1(SourceGPUCommandProjection *s,uint32_t word) {
    s->dispatch.kind=SOURCE_GPU_DISPATCH_NONE;
    if(s->error)return 0;
    unsigned command=word>>24;
    if(command==0 || command==1) {
        if(s->budget<0)s->budget=0;
        s->count=0;s->phase=0;s->transfer_words=0;
        if(!command) {
            s->clip_x0=s->clip_y0=s->clip_x1=s->clip_y1=0;
            s->offset_x=s->offset_y=0;s->draw_mode=s->texture_window=s->mask_bits=0;
            s->display_mode=s->dma_direction=0;
        }
    } else if(command==4)s->dma_direction=word&3u;
    else if(command==8) {
        if(word&8u){s->error=SOURCE_GPU_COMMAND_UNSUPPORTED;return 0;}
        s->display_mode=word&255u;
    } else if(command!=2 && command!=3 && command!=5 && command!=6 && command!=7 && command!=0x10) {
        s->error=SOURCE_GPU_COMMAND_UNSUPPORTED;return 0;
    }
    return 1;
}

static inline void source_gpu_command_read(SourceGPUCommandProjection *s) {
    s->dispatch.kind=SOURCE_GPU_DISPATCH_NONE;
    if(s->phase==8 && --s->transfer_words==0)s->phase=0;
}

static inline int source_gpu_command_write(SourceGPUCommandProjection *s,uint32_t word) {
    s->dispatch.kind=SOURCE_GPU_DISPATCH_NONE;
    if(s->error) return 0;
    /* Idle FIFO extension follows the front command's feedback length;
     * active split quads have no extension. Overflow is a scope stop. */
    if(s->count>=16 && (s->phase || s->count-16>=source_gpu_command_feedback_length(s->queue[0]))) {
        s->error=SOURCE_GPU_COMMAND_OVERFLOW; return 0;
    }
    s->queue[s->count++]=word;
    return source_gpu_command_process(s);
}

static inline int source_gpu_command_update(SourceGPUCommandProjection *s,uint64_t cycle) {
    s->dispatch.kind=SOURCE_GPU_DISPATCH_NONE;
    if(s->error) return 0;
    if(cycle<s->last_update) { s->error=SOURCE_GPU_COMMAND_REVERSE_TIME; return 0; }
    uint64_t elapsed=cycle-s->last_update;
    if(!elapsed) return 1;
    int64_t missing=256-(int64_t)s->budget;
    s->budget=elapsed>=(uint64_t)((missing+1)/2) ? 256 : s->budget+(int32_t)(2*elapsed);
    s->last_update=cycle;
    return source_gpu_command_process(s);
}
#endif
