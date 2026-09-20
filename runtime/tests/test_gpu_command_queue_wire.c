#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu_command_queue_wire.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    SourceGPUCommandProjection live={0},restored={0},untouched={0};
    live.field_valid=1;live.clip_x1=1023;live.clip_y1=511;
    uint32_t quad[]={0x280000ff,0x000a000a,0x000a0020,0x0020000a,0x00200020};
    for(unsigned i=0;i<5;i++)assert(source_gpu_command_write(&live,quad[i]));
    assert(live.phase==2 && live.count==1 && live.first_triangles==1);
    live.last_update=100;
    uint8_t bytes[GPU_COMMAND_QUEUE_WIRE_BYTES];PstW w;PstR r;
    pst_w_init(&w,bytes,sizeof(bytes));assert(gpu_command_queue_write(&w,&live));
    assert(w.written==sizeof(bytes));
    pst_r_init(&r,bytes,sizeof(bytes));assert(gpu_command_queue_read(&r,&restored));
    assert(!memcmp(&live,&restored,sizeof(live)));
    assert(source_gpu_command_update(&live,1000));
    assert(source_gpu_command_update(&restored,1000));
    assert(live.dispatch.kind==SOURCE_GPU_DISPATCH_QUAD_SECOND && live.second_triangles==1);
    assert(!memcmp(&live,&restored,sizeof(live)));
    for(unsigned n=0;n<sizeof(bytes);n++) {
        pst_r_init(&r,bytes,n);restored=untouched;
        assert(!gpu_command_queue_read(&r,&restored));
        assert(!memcmp(&restored,&untouched,sizeof(restored)));
    }
    bytes[12]=33; /* queue count exceeds storage */
    pst_r_init(&r,bytes,sizeof(bytes));
    assert(!gpu_command_queue_read(&r,&restored));
    /* A transfer phase without a remaining word would underflow on read. */
    SourceGPUCommandProjection invalid={0};invalid.phase=8;
    pst_w_init(&w,bytes,sizeof(bytes));assert(gpu_command_queue_write(&w,&invalid));
    pst_r_init(&r,bytes,sizeof(bytes));assert(!gpu_command_queue_read(&r,&restored));
    invalid.phase=2;invalid.command=0x20;
    pst_w_init(&w,bytes,sizeof(bytes));assert(gpu_command_queue_write(&w,&invalid));
    pst_r_init(&r,bytes,sizeof(bytes));assert(!gpu_command_queue_read(&r,&restored));
    puts("PASS: pending quad resumes once; truncated/invalid queue is rejected transactionally");
    return 0;
}
