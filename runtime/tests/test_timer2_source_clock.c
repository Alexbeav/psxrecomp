#include "timer2_source_clock.h"
#include <stdio.h>
int main(void){
 PsxTimer2Source s;unsigned op,v,c,m,t,d,done,irq,next,ret,rows=0,errors=0,pulses=0;int counting;
 while(scanf("%u %u %u %u %u %u %u %d %u %u %u",&op,&v,&c,&m,&t,&d,&done,&counting,&irq,&next,&ret)==11){
  unsigned value=0;
  if(op==0){timer2_source_reset(&s);pulses=0;}
  else if(op==1)pulses+=timer2_source_write(&s,4,(uint16_t)v);
  else if(op==2)pulses+=timer2_source_write(&s,0,(uint16_t)v);
  else if(op==3)pulses+=timer2_source_write(&s,8,(uint16_t)v);
  else if(op==4)pulses+=timer2_source_cpu(&s,v);
  else if(op==5)value=timer2_source_read(&s,4);
  if(s.counter!=c || s.mode!=m || s.target!=t || s.divider!=d || s.irq_done!=(int)done || s.counting!=counting || pulses!=irq || timer2_source_next(&s)!=next || value!=ret){
   if(errors<4)fprintf(stderr,"row%u op%u/%u counter%u/%u mode%x/%x divider%u/%u irq%u/%u next%u/%u\n",rows,op,v,s.counter,c,s.mode,m,s.divider,d,pulses,irq,timer2_source_next(&s),next);errors++;
  }rows++;
 }
 printf("%u source states; %u mismatches\n",rows,errors);return errors?1:0;
}
