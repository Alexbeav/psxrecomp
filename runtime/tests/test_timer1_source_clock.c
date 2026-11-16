/* Authored event sequences; expected states come from the independent exact
 * source oracle retained in the private qualification receipt. */
#include "timer1_source_clock.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    PsxTimer1Source s;unsigned op,v,c,m,t,b,irq;int counting;unsigned rows=0;
    while(scanf("%u %u %u %u %u %d %u %u",&op,&v,&c,&m,&t,&counting,&b,&irq)==8) {
        if(op==0) timer1_source_reset(&s);
        else if(op==1) assert(timer1_source_write(&s,4,(uint16_t)v));
        else if(op==2) assert(timer1_source_write(&s,0,(uint16_t)v));
        else if(op==3) assert(timer1_source_write(&s,8,(uint16_t)v));
        else if(op==4) timer1_source_blank(&s,!!v);
        else if(op==5) timer1_source_hblank(&s,v);
        else if(op==6) timer1_source_cpu(&s,v);
        else if(op==7) (void)timer1_source_read(&s,4);
        else assert(0);
        if(s.counter!=c || s.mode!=m || s.target!=t || s.counting!=counting || s.blank!=(int)b || irq) {
            fprintf(stderr,"row %u op%u value%u: counter%u/%u mode%x/%x target%u/%u counting%d/%d blank%d/%u irq%u\n",rows,op,v,s.counter,c,s.mode,m,s.target,t,s.counting,counting,s.blank,b,irq);return 1;
        }
        ++rows;
    }
    assert(rows==2400);
    PsxTimer1Source before=s;
    assert(!timer1_source_write(&s,4,0x158));assert(!memcmp(&s,&before,sizeof s));
    assert(!timer1_source_write(&s,4,0x168));assert(!memcmp(&s,&before,sizeof s));
    printf("timer1 source clock: %u exact-source states and IRQ rejection PASS\n",rows);
}
