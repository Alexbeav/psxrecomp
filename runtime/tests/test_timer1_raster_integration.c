#include "timers.h"
#include "input_route_raster_clock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void psx_irq_raise(int source,unsigned detail) { (void)source;(void)detail; assert(0); }
void event_ring_record_aux(int kind,unsigned char source,unsigned aux) { (void)kind;(void)source;(void)aux; }
static uint64_t now;
static void advance(InputRouteRasterClock *r,uint64_t to) {
    assert(to>=now && to-now<=UINT32_MAX);
    input_route_raster_advance_observed(r,(uint32_t)(to-now),timers_source_raster_event,NULL);
    timers_source_raster_finish(to);now=to;
}
int main(int argc,char **argv) {
    assert(argc==2);timers_init();
    if(!strcmp(argv[1],"default")) {
        assert(!timers_source_raster_enabled());
        timers_write(0x1f801114,0x148);timers_advance(2145);assert(timers_read(0x1f801110)==0);
        timers_advance(1);assert(timers_read(0x1f801110)==1);puts("default2146 PASS");return 0;
    }
    assert(timers_source_raster_enabled());
    if(!strcmp(argv[1],"irq")) {timers_write(0x1f801114,0x158);return 9;}
    if(!strcmp(argv[1],"restore")) {uint16_t a[3]={0};uint32_t b[3]={0};int32_t c[3]={0};timers_set_snapshot(a,b,a,c,b);return 9;}
    InputRouteRasterClock r;input_route_raster_reset(&r);
    /* Independent exact-source scalar checkpoint and observed reads. No RAM,
     * executable, game input or reference implementation text in this test. */
    r.cycle=now=621642115;r.remaining=3203;r.phase=0;r.alternate=1;
    r.fraction=34184;r.scanline=256;r.lines=263;r.mode=100;r.blank=1;r.field=0;
    timers_source_raster_finish(now);
    timers_write(0x1f801118,65535);timers_write(0x1f801114,0x148);timers_write(0x1f801110,249);
    advance(&r,622400946);assert(timers_read(0x1f801110)==601);
    advance(&r,622400989);assert(timers_read(0x1f801110)==601);
    /* Repeated reads cannot advance or clear count; IRQ mode rejection above
     * does not silently inherit native IRQ behavior. */
    assert(timers_read(0x1f801110)==601);
    uint16_t counters[3],targets[3];uint32_t modes[3],fractions[3];int32_t irqs[3];
    timers_get_snapshot(counters,modes,targets,irqs,fractions);
    assert(counters[1]==601 && (modes[1]&1023)==0x148 && targets[1]==65535);
    /* At the next H edge a write at the same timestamp follows that edge. */
    uint64_t edge=now+((uint64_t)r.remaining*65536-r.fraction+103895)/103896;
    assert(r.phase==0);advance(&r,edge);assert(timers_read(0x1f801110)==602);
    timers_write(0x1f801110,7);assert(timers_read(0x1f801110)==7);
    advance(&r,edge);assert(timers_read(0x1f801110)==7);
    puts("source timer1 production raster/read/write ordering PASS");
}
