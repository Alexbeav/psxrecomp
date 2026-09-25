/* Scalar timing projection, not scheduler or renderer qualification. */
#include "source_gpu_command_projection.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void setup(SourceGPUCommandProjection *s) {
    source_gpu_command_cold(s);
    assert(source_gpu_command_write(s,0xe3000000));
    assert(source_gpu_command_write(s,0xe407ffff));
    assert(source_gpu_command_update(s,128));
}
static void quad(SourceGPUCommandProjection *s,unsigned n) {
    assert(source_gpu_command_write(s,0x280000ff));
    assert(source_gpu_command_write(s,0));
    assert(source_gpu_command_write(s,n));
    assert(source_gpu_command_write(s,n<<16));
    assert(source_gpu_command_write(s,n|(n<<16)));
}
static void report(const SourceGPUCommandProjection *s,const char *label) {
    printf("%s %d %u %u %d %llu %u %u\n",label,s->budget,s->phase,
        s->count,source_gpu_command_ready(s),(unsigned long long)s->last_update,
        s->command,s->count?s->queue[0]:0);
}
int main(int argc,char **argv) {
    SourceGPUCommandProjection s;
    if(argc==3) {
        unsigned n=(unsigned)atoi(argv[1]),chain=(unsigned)atoi(argv[2]);
        setup(&s); report(&s,"before_start");
        assert(source_gpu_command_update(&s,146)); quad(&s,n);
        if(chain && source_gpu_command_ready(&s))
            for(unsigned i=0;i<6;++i) assert(source_gpu_command_write(&s,0));
        report(&s,"after_start"); report(&s,"immediate_state");
        assert(source_gpu_command_update(&s,256));
        assert(source_gpu_command_update(&s,384));
        if(chain && n==32) {
            assert(source_gpu_command_ready(&s));
            for(unsigned i=0;i<6;++i) assert(source_gpu_command_write(&s,0));
        }
        report(&s,"before_release");
        for(unsigned cycle=512;cycle<=768;cycle+=128) assert(source_gpu_command_update(&s,cycle));
        report(&s,"final_state");
        /* Empty queue: final explicit service reproduces the retained scalar
         * state, without claiming the intervening raster deadline sequence. */
        assert(source_gpu_command_update(&s,13056)); report(&s,"settled_state");
        assert(s.first_triangles==1 && s.second_triangles==1); return 0;
    }
    /* Costs and FIFO rules below follow the documented model:
     * No$PSX "GPU Rendering Timings" (sha256 a3b2131f3774...) and "GPU FIFO"
     * (sha256 5767a2b3a5c8...), snapshot Z:/Share/psxrecomp/evidence/T172/
     * gpu-timing-source-2026-09-25; PSX-SPX a253f078 "Ready Bits". One budget
     * unit is one CPU clock of credit. */
    setup(&s); assert(source_gpu_command_update(&s,146)); quad(&s,32);
    assert(s.budget==-200 && s.phase==2 && s.count==1); /* No$ Polygons: 146 credit - 346 */
    assert(source_gpu_command_update(&s,146)); /* Same-time call cannot drain. */
    assert(s.second_triangles==0);
    assert(source_gpu_command_update(&s,345)); assert(s.budget==-1 && s.phase==2); /* 1 credit per CPU clock */
    assert(source_gpu_command_update(&s,346)); /* Exact zero-credit boundary. */
    assert(s.budget==-329 && s.phase==0 && source_gpu_command_ready(&s)==0); /* Ready Bits: 0 while executing */
    assert(source_gpu_command_write(&s,0)); assert(s.budget==-329);
    quad(&s,1); /* Negative budget queues a complete next quad. */
    assert(s.count==5 && s.phase==0 && source_gpu_command_ready(&s)==0);
    assert(source_gpu_command_update(&s,675));
    assert(s.first_triangles==2 && s.second_triangles==1 && s.count==1 && s.budget==-15);
    assert(source_gpu_command_update(&s,675)); assert(s.second_triangles==1);
    assert(source_gpu_command_update(&s,690)); assert(s.second_triangles==2 && s.budget==-11);
    assert(!source_gpu_command_update(&s,689)); assert(s.error==SOURCE_GPU_COMMAND_REVERSE_TIME);
    /* Poly-lines are documented (PSX-SPX "GPU Render Line Commands"); E7h is
     * outside the modelled command set. */
    setup(&s); assert(!source_gpu_command_write(&s,0xe7000000));
    assert(s.error==SOURCE_GPU_COMMAND_UNSUPPORTED);
    setup(&s); assert(source_gpu_command_update(&s,146)); quad(&s,32);
    for(unsigned i=1;i<16;++i) assert(source_gpu_command_write(&s,0));
    assert(!source_gpu_command_write(&s,0)); assert(s.error==SOURCE_GPU_COMMAND_OVERFLOW);
    setup(&s); assert(source_gpu_command_write(&s,0x28000000));
    assert(source_gpu_command_write(&s,0)); assert(source_gpu_command_write(&s,0x10001));
    assert(source_gpu_command_write(&s,0x10000)); /* General flat first triangle. */
    setup(&s); assert(source_gpu_command_write(&s,0x28000000));
    assert(source_gpu_command_write(&s,0)); assert(source_gpu_command_write(&s,1));
    assert(source_gpu_command_write(&s,0x10000));
    assert(source_gpu_command_write(&s,0x10002)); /* Independent fourth vertex. */
    setup(&s); assert(source_gpu_command_write(&s,0xe4000000));
    assert(source_gpu_command_write(&s,0x28000000));
    assert(source_gpu_command_write(&s,0)); assert(source_gpu_command_write(&s,1));
    assert(source_gpu_command_write(&s,0x10000)); /* Inclusive clip admits one pixel. */
    assert(source_gpu_command_write(&s,0x10001)); assert(s.budget==102); /* No$ Polygons */
    setup(&s); assert(source_gpu_command_write(&s,0xe4080000)); /* Clip Y512 is not admitted. */
    assert(source_gpu_command_write(&s,0x28000000));
    assert(source_gpu_command_write(&s,0)); assert(source_gpu_command_write(&s,1));
    assert(!source_gpu_command_write(&s,0x10000));
    setup(&s); assert(source_gpu_command_write(&s,0xe5000001)); /* Offset X1. */
    assert(source_gpu_command_write(&s,0x28000000));
    assert(source_gpu_command_write(&s,0x000003ff)); /* Offset makes X1024: qualified source clipping. */
    assert(source_gpu_command_write(&s,0x000003fe));
    assert(source_gpu_command_write(&s,0x000103ff));
    assert(s.phase==2 && s.budget==113); /* One clipped pixel at X1023; No$ Polygons. */
    /* No$ "GPU FIFO": while drawing is busy, FLUSHCACHE/TEXPAGE/TEXWINDOW take
     * one prefetch word and wait (16 FIFO words + 1), and MASKBITS runs at once. */
    const uint32_t ordinary[]={0x01000000,0xe1000200,0xe2007fff,0xe6000001};
    for(unsigned c=0;c<sizeof(ordinary)/sizeof(ordinary[0]);++c) {
        unsigned waits=c!=3;
        setup(&s);s.budget=-100;
        assert(source_gpu_command_write(&s,ordinary[c]));assert(source_gpu_command_ready(&s)==0);
        assert(source_gpu_command_write(&s,ordinary[c]));assert(source_gpu_command_ready(&s)==0);
        assert(s.draw_mode==0 && s.texture_window==0 && s.mask_bits==(waits?0u:1u));
        for(unsigned i=2;i<17;i++)assert(source_gpu_command_write(&s,ordinary[c]));
        assert(s.count==(waits?17u:0u) && s.budget==-100);
        if(waits){assert(!source_gpu_command_write(&s,ordinary[c]));assert(s.error==SOURCE_GPU_COMMAND_OVERFLOW);}
        setup(&s);s.budget=-2;
        assert(source_gpu_command_write(&s,ordinary[c]));
        assert(s.count==waits && s.budget==-2);
        assert(source_gpu_command_update(&s,130)); /* Exactly zero admits one word. */
        assert(s.count==0 && s.budget==(waits?-2:0));
        if(c==1)assert(s.draw_mode==0x200);
        if(c==2)assert(s.texture_window==0x7fff);
        if(c==3)assert(s.mask_bits==1);
        assert(source_gpu_command_write(&s,ordinary[c]));
        assert(s.count==waits);
        assert(source_gpu_command_gp1(&s,0x01000000));
        assert(s.count==0 && s.budget==0 && s.last_update==130);
    }
    const uint32_t immediate[]={0,0xe3000000,0xe407ffff,0xe5000000};
    for(unsigned c=0;c<sizeof(immediate)/sizeof(immediate[0]);++c) {
        setup(&s);s.budget=-2;
        assert(source_gpu_command_write(&s,immediate[c]));
        assert(s.count==0 && s.budget==-2);
    }
    /* Rendering consumers get one original packet phase, including the
     * prefix before the last vertex arrives. Reset cannot publish a suffix. */
    setup(&s);
    const uint32_t packet[]={0x280000ff,0,32,32u<<16,32u|(32u<<16)};
    for(unsigned i=0;i<4;i++)assert(source_gpu_command_write(&s,packet[i]));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_QUAD_FIRST && s.dispatch.count==4);
    assert(memcmp(s.dispatch.words,packet,4*sizeof(uint32_t))==0);
    assert(source_gpu_command_write(&s,packet[4]));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_NONE);
    assert(source_gpu_command_gp1(&s,0x01000000));
    assert(source_gpu_command_update(&s,1024));
    assert(!s.dispatch.kind && s.first_triangles==1 && s.second_triangles==0);
    setup(&s);
    const uint32_t empty[]={0x280000ff,0,0,0,0};
    for(unsigned i=0;i<4;i++)assert(source_gpu_command_write(&s,empty[i]));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_QUAD_FIRST);
    assert(source_gpu_command_write(&s,empty[4]));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_QUAD_SECOND && s.dispatch.count==5);
    assert(memcmp(s.dispatch.words,empty,sizeof(empty))==0);
    assert(source_gpu_command_update(&s,128));
    assert(!s.dispatch.kind && s.first_triangles==1 && s.second_triangles==1);
    puts("PASS exact-zero/negative credit, split dispatch, reset cancellation, queue and scoped rejection");
    return 0;
}
