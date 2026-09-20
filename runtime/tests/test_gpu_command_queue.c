#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu_command_queue.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* A shaded textured quad's second triangle keeps CLUT/page metadata,
     * but uses vertices/colors 1,2,3. It must not reopen a four-vertex packet. */
    SourceGPUCommandDispatch d={SOURCE_GPU_DISPATCH_QUAD_SECOND,12,
        {0x3c112233,0x00100011,0x12340102,0x00445566,0x00200021,0x45670304,
         0x00778899,0x00300031,0xaaaa0506,0x00aabbcc,0x00400041,0xbbbb0708}};
    const uint32_t expected[]={0x34445566,0x00200021,0x12340304,
        0x00778899,0x00300031,0x45670506,0x00aabbcc,0x00400041,0xbbbb0708};
    uint32_t packet[12];
    assert(gpu_command_queue_packet(&d,packet)==9);
    assert(!memcmp(packet,expected,sizeof(expected)));
    d.kind=SOURCE_GPU_DISPATCH_QUAD_FIRST;d.count=9;
    assert(gpu_command_queue_packet(&d,packet)==9);
    assert(packet[0]==0x34112233 && packet[1]==0x00100011 && packet[7]==0x00300031);
    d.kind=SOURCE_GPU_DISPATCH_COMMAND;d.count=4;d.words[0]=0x5a112233;
    assert(gpu_command_queue_packet(&d,packet)==4 && packet[0]==0x52112233);

    SourceGPUCommandProjection q={0};
    assert(gpu_command_queue_deadline(&q)==UINT32_MAX);
    q.queue[0]=0x200000ff;q.count=3;q.budget=-9;
    assert(gpu_command_queue_deadline(&q)==UINT32_MAX); /* incomplete triangle */
    q.count=4;
    assert(gpu_command_queue_deadline(&q)==5);
    q.budget=0;assert(gpu_command_queue_deadline(&q)==1);
    q.phase=8;assert(gpu_command_queue_deadline(&q)==UINT32_MAX);
    q.phase=4;q.budget=-100;assert(gpu_command_queue_deadline(&q)==1);
    q.phase=0;q.count=1;q.queue[0]=0xe3000000;
    assert(gpu_command_queue_deadline(&q)==1); /* clipping command bypasses draw debt */
    puts("PASS: complete primitive delivery and incomplete/debt/readback deadlines");
    return 0;
}
