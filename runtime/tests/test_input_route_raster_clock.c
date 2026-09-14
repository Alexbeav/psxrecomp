#include "input_route_raster_clock.h"
#include <assert.h>
#include <stdio.h>

static void next_line(InputRouteRasterClock *s) {
    uint32_t line=s->scanline;
    while (s->scanline==line) input_route_raster_advance(s,1);
}

static void status_cases(void) {
    InputRouteRasterClock s;
    input_route_raster_reset(&s);
    assert(input_route_raster_status(&s)==0x2000u);
    while (s.scanline!=16) next_line(&s);
    assert(!s.blank && !(input_route_raster_status(&s)>>31));
    next_line(&s); assert(input_route_raster_status(&s)==0x80002000u);
    next_line(&s); assert(input_route_raster_status(&s)==0x2000u);
    input_route_raster_reset(&s);
    assert(input_route_raster_gp1(&s,0x08000024));
    while (s.scanline!=256) next_line(&s);
    assert(s.blank && s.readout_field==1 && input_route_raster_status(&s)==0x2000u);
    while (s.scanline!=262) next_line(&s);
    assert(s.field==1 && s.blank && input_route_raster_status(&s)==0);
    while (s.scanline!=16) next_line(&s);
    assert(!s.blank && input_route_raster_status(&s)==0x80000000u);
    next_line(&s); assert(input_route_raster_status(&s)==0x80000000u);
    assert(input_route_raster_gp1(&s,0x05000400));
    assert(input_route_raster_status(&s)==0x80000000u); /* next scanline owns readout */
    next_line(&s); assert(input_route_raster_status(&s)==0);
    uint32_t offset=s.y_offset, readout=s.readout_y, field=s.readout_field;
    assert(input_route_raster_gp1(&s,0));
    assert(s.y_start==0 && s.y_offset==offset && s.readout_y==readout && s.readout_field==field);
    puts("raster field/parity independence, progressive lines, interlace blanking, display origin and soft reset pass");
}

int main(int argc, char **argv) {
    InputRouteRasterClock a,b;
    if (argc==2) {
        FILE *f=fopen(argv[1],"r"); assert(f);
        char op; unsigned long long cycle; unsigned word; unsigned samples=0;
        input_route_raster_reset(&a);
        while (fscanf(f," %c",&op)==1) {
            if (op=='A') {
                assert(fscanf(f,"%llu",&cycle)==1 && cycle>=a.cycle && cycle-a.cycle<=UINT32_MAX);
                input_route_raster_advance(&a,(uint32_t)(cycle-a.cycle));
            } else if (op=='G') {
                assert(fscanf(f,"%x",&word)==1 && input_route_raster_gp1(&a,word));
            } else if (op=='Q') {
                assert(fscanf(f,"%x",&word)==1 && input_route_raster_status(&a)==word);
                samples++;
            } else {
                assert(op=='S');
                uint32_t expected[11];
                uint32_t actual[11]={a.mode,a.start,a.end,a.blank,a.lines,a.scanline,a.field,a.phase,a.remaining,a.fraction,103896};
                for (unsigned i=0;i<11;i++) assert(fscanf(f,"%u",&expected[i])==1);
                assert(!memcmp(expected,actual,sizeof(actual))); samples++;
            }
        }
        assert(!ferror(f)); fclose(f); printf("source raster scalar samples pass: %u\n",samples); return 0;
    }
    input_route_raster_reset(&a); b=a;
    assert(input_route_raster_until_rise(&a)==551053);
    input_route_raster_advance(&a,551052); assert(!a.rises);
    input_route_raster_advance(&a,1); assert(a.rises==1 && a.last_rise==551053);
    /* Independent one-CPU-clock stepping versus event-sized batches. */
    for (uint32_t i=0;i<551053;i++) input_route_raster_advance(&b,1);
    assert(!memcmp(&a,&b,sizeof(a)));
    for (uint32_t i=0;i<400;i++) {
        uint32_t word=i%7==0 ? 0 : i%3==0 ? 0x08000024u : 0x08000000u;
        assert(input_route_raster_gp1(&a,word)); assert(input_route_raster_gp1(&b,word));
        uint32_t span=1000u+(i*7919u)%20000u;
        input_route_raster_advance(&a,span);
        for (uint32_t j=0;j<span;j++) input_route_raster_advance(&b,1);
        assert(!memcmp(&a,&b,sizeof(a)));
    }
    InputRouteRasterClock before=a;
    assert(!input_route_raster_gp1(&a,0x08000008));
    assert(!memcmp(&a,&before,sizeof(a)));
    /* GPU soft reset restores control values without resetting beam phase. */
    assert(input_route_raster_gp1(&a,0));
    assert(a.cycle==before.cycle && a.fraction==before.fraction && a.scanline==before.scanline && a.field==before.field);
    /* Changes behind the beam take effect at the next scanline match. */
    input_route_raster_reset(&a);input_route_raster_advance(&a,500000);
    assert(input_route_raster_gp1(&a,0x0703F010));
    uint32_t distance=input_route_raster_until_rise(&a); assert(distance>0 && distance<100000);
    uint32_t count=a.rises;input_route_raster_advance(&a,distance-1);assert(a.rises==count);
    input_route_raster_advance(&a,1);assert(a.rises==count+1);
    assert(input_route_raster_gp1(&a,0x07000000));
    assert(input_route_raster_until_rise(&a)==UINT32_MAX);
    status_cases();
    puts("raster clock chunking, deadline, mode, soft reset, range and PAL guards pass");
    return 0;
}
