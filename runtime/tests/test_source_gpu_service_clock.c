#include "source_gpu_service_clock.h"
#include "source_gpu_command_projection.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct Harness {
    SourceGPUCommandProjection command;
    unsigned calls,duplicate_calls,first_phases,second_phases;
    uint64_t last;
} Harness;
static void service(void *context,uint64_t cycle,unsigned kind) {
    Harness *h=context; (void)kind;
    unsigned before=h->command.first_triangles+h->command.second_triangles;
    assert(source_gpu_command_update(&h->command,cycle)); ++h->calls;
    if(cycle==h->last) {
        ++h->duplicate_calls;
        assert(h->command.first_triangles+h->command.second_triangles==before);
    }
    h->last=cycle;
}
static void report(Harness *h) {
    SourceGPUCommandProjection *s=&h->command;
    printf("%llu %u %u %u %u %u %u\n",(unsigned long long)s->last_update,
        (uint32_t)s->budget,s->phase,s->count,(unsigned)source_gpu_command_ready(s),
        s->command,s->count?s->queue[0]:0);
}
static void quad(Harness *h,unsigned n) {
    uint32_t words[]={0x280000ff,0,n,n<<16,n|(n<<16)};
    for(unsigned i=0;i<5;++i) assert(source_gpu_command_write(&h->command,words[i]));
}
int main(int argc,char **argv) {
    assert(argc==3 || argc==6);
    unsigned n=(unsigned)atoi(argv[1]),chain=(unsigned)atoi(argv[2]);
    SourceGPUServiceClock clock;source_gpu_service_cold(&clock);
    Harness h={0};source_gpu_command_cold(&h.command);
    assert(source_gpu_command_write(&h.command,0xe3000000));
    assert(source_gpu_command_write(&h.command,0xe407ffff));report(&h);
    /* Ordinary authored DMA register writes, independently observed before
     * the start. These are inputs, not inferred from the model's deadlines. */
    uint64_t writes[]={98,108,120,121};
    for(unsigned i=0;i<4;++i) {
        assert(source_gpu_service_dma_write(&clock,writes[i],service,&h));report(&h);
    }
    assert(source_gpu_service_to(&clock,128,service,&h));report(&h);
    assert(h.duplicate_calls==1); /* Own GPU event + periodic DMA at 128. */
    assert(source_gpu_service_dma_write(&clock,146,service,&h));quad(&h,n);
    unsigned pending=chain;
    if(pending && source_gpu_command_ready(&h.command)) {
        for(unsigned i=0;i<6;++i) assert(source_gpu_command_write(&h.command,0));
        pending=0;
    }
    report(&h);
    uint64_t ends[2]={0,0},limit=13171;unsigned frame=0;
    if(argc==6) {ends[0]=strtoull(argv[3],0,10);ends[1]=strtoull(argv[4],0,10);limit=ends[1];}
    while(source_gpu_service_next(&clock)<=limit || (argc==6 && frame<2)) {
        uint64_t next=source_gpu_service_next(&clock);
        int boundary=argc==6 && frame<2 && ends[frame]<=next;
        if(boundary) next=ends[frame];
        assert(source_gpu_service_to(&clock,next,service,&h));
        if(boundary) {
            if(atoi(argv[5])) assert(source_gpu_service_frame_end(&clock,next,service,&h));
            ++frame;
        }
        if(pending && next%128==0 && source_gpu_command_ready(&h.command)) {
            for(unsigned i=0;i<6;++i) assert(source_gpu_command_write(&h.command,0));
            pending=0;
        }
        report(&h);
    }
    assert(!pending && h.command.first_triangles==1 && h.command.second_triangles==1);
    assert(h.duplicate_calls>1);
    /* Explicit repeated caller at an existing deadline is not another phase. */
    unsigned phases=h.command.first_triangles+h.command.second_triangles;
    assert(source_gpu_service_dma_write(&clock,clock.cycle,service,&h));
    assert(h.command.first_triangles+h.command.second_triangles==phases);
    assert(!source_gpu_service_to(&clock,clock.cycle-1,service,&h));
    return 0;
}
