#include "timers.h"
#include <stdio.h>
#include <stdint.h>
void psx_irq_raise(int source,unsigned detail){(void)source;(void)detail;}
void event_ring_record_aux(int kind,unsigned char source,unsigned aux){(void)kind;(void)source;(void)aux;}
int main(void){
 unsigned op,v,c,m,t,d,done,irq,next,ret,rows=0,failures=0;int counting;
 while(scanf("%u %u %u %u %u %u %u %d %u %u %u",&op,&v,&c,&m,&t,&d,&done,&counting,&irq,&next,&ret)==11){
  if(op==0)timers_init();else if(op==1)timers_write(0x1f801124,v);else if(op==2)timers_write(0x1f801120,v);else if(op==3)timers_write(0x1f801128,v);else if(op==4)timers_advance(v);else if(op==5)(void)timers_read(0x1f801124);
  uint16_t counters[3],targets[3];uint32_t modes[3],fractions[3];int32_t lines[3];timers_get_snapshot(counters,modes,targets,lines,fractions);
  if(counters[2]!=c||fractions[2]!=d){if(failures<4)fprintf(stderr,"row%u op%u/%u count%u/%u divider%u/%u\n",rows,op,v,counters[2],c,fractions[2],d);failures++;}rows++;
 }
 printf("%u rows; %u counter/divider mismatches\n",rows,failures);return failures?1:0;
}
