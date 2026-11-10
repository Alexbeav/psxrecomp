/* Fixed-range source frame request stream, independent of hardware VBlank.
 * CPU origin is a retained authored uncached-loop pre-fetch; later steps cost
 * five clocks, as independently qualified against actual CPU execution. */
#include "source_gpu_service_clock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static SourceGPUServiceClock *active;
static void service(void *context,uint64_t cycle,unsigned kind) {
    (void)context;
    if(kind==SOURCE_GPU_EVENT_FRAME_END)
        printf("frame %llu %llu %u\n",(unsigned long long)cycle,
            (unsigned long long)active->frame_request_cycle,active->raster.rises);
}
int main(int argc,char **argv) {
    assert(argc==4);SourceGPUServiceClock clock;source_gpu_service_cold(&clock);active=&clock;
    assert(input_route_raster_gp1(&clock.raster,0x07000010u|((unsigned)atoi(argv[1])<<10)));
    uint64_t cycle=strtoull(argv[2],0,10),limit=strtoull(argv[3],0,10);
    assert(source_gpu_service_to(&clock,cycle,service,0));assert(!clock.frame_pending);
    while(cycle<=limit && clock.frame_returns<2) {
        assert(source_gpu_service_cpu_boundary(&clock,cycle,service,0));
        unsigned before=clock.frame_returns;
        assert(source_gpu_service_cpu_boundary(&clock,cycle,service,0));
        assert(clock.frame_returns==before); /* same timestamp cannot consume twice */
        cycle+=5;
    }
    assert(clock.frame_returns==2);return 0;
}
