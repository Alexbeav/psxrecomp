/* Source-observed late quad, with an automatic CPU-step frame return. */
#include "source_gpu_service_clock.h"
#include "source_gpu_command_projection.h"
#include <assert.h>
#include <stdio.h>
typedef struct Harness { SourceGPUCommandProjection command; unsigned duplicates; uint64_t last; } Harness;
static void service(void *context,uint64_t cycle,unsigned kind) {
    Harness *h=context;unsigned phases=h->command.first_triangles+h->command.second_triangles;
    assert(source_gpu_command_update(&h->command,cycle));
    if(cycle==h->last) {h->duplicates++;assert(phases==h->command.first_triangles+h->command.second_triangles);}
    h->last=cycle;
    if(cycle>=1116900 && cycle<=1117800)
        printf("state %llu %u %u %u %u %u %u %u\n",(unsigned long long)cycle,(uint32_t)h->command.budget,h->command.phase,h->command.count,(unsigned)source_gpu_command_ready(&h->command),h->command.command,h->command.count?h->command.queue[0]:0,kind);
}
int main(void) {
    SourceGPUServiceClock clock;source_gpu_service_cold(&clock);Harness h={0};source_gpu_command_cold(&h.command);
    assert(source_gpu_command_write(&h.command,0xe3000000));assert(source_gpu_command_write(&h.command,0xe407ffff));
    const uint64_t writes[]={98,108,120,121,1117005};
    for(unsigned i=0;i<5;i++)assert(source_gpu_service_dma_write(&clock,writes[i],service,&h));
    const uint32_t quad[]={0x280000ff,0,32,32u<<16,32u|(32u<<16)};
    for(unsigned i=0;i<5;i++)assert(source_gpu_command_write(&h.command,quad[i]));
    assert(h.command.budget==-356 && h.command.phase==2 && h.command.count==1);
    /* Actual source cached NOP steps: warm line through 66C, cold fetch at
     * 670 from 1,117,170, followed by its next CPU boundary at 1,117,178.
     * This standalone fixture supplies that independently observed step;
     * actual generated/interpreted scheduler ownership is a separate gate. */
    const uint64_t steps[]={1117167,1117168,1117169,1117170,1117178};
    for(unsigned i=0;i<5;i++)assert(source_gpu_service_cpu_boundary(&clock,steps[i],service,&h));
    assert(clock.frame_returns==1 && clock.frame_request_cycle==1117175);
    assert(h.command.budget==-10 && h.command.phase==2 && h.command.count==1 && !source_gpu_command_ready(&h.command));
    unsigned before=h.command.first_triangles+h.command.second_triangles;
    assert(source_gpu_service_cpu_boundary(&clock,1117178,service,&h));
    assert(clock.frame_returns==1 && before==h.command.first_triangles+h.command.second_triangles);
    assert(source_gpu_service_to(&clock,1117800,service,&h));
    assert(h.command.second_triangles==1 && source_gpu_command_ready(&h.command));
    assert(h.duplicates);return 0;
}
