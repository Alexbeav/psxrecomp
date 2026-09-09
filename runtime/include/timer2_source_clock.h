#ifndef PSX_TIMER2_SOURCE_CLOCK_H
#define PSX_TIMER2_SOURCE_CLOCK_H
#include <stdint.h>
#include <string.h>
/* Explicit original-Octoshock compatibility state. Independent expression of
 * the retained source transitions, qualified against an external scalar oracle.
 * This is not a claim that every original-core quirk describes PS1 hardware.
 * No guest memory, title addresses, input words or selected timing constants. */
typedef struct PsxTimer2Source {
    uint32_t counter,mode,target,divider;
    int irq_done,counting;
} PsxTimer2Source;
static inline void timer2_source_reset(PsxTimer2Source *s){memset(s,0,sizeof(*s));}
static inline int timer2_source_target(PsxTimer2Source *s,unsigned *pulses){
    s->mode|=0x800;
    if(s->mode&8) s->counter%=s->target?s->target:1;
    if(!(s->mode&0x10) || s->irq_done) return 0;
    s->irq_done=1;(*pulses)++;
    return !s->counter || s->counter==s->target;
}
static inline unsigned timer2_source_cpu(PsxTimer2Source *s,uint32_t cycles){
    unsigned pulses=0;
    /* Original TIMER_Update skips this encoding even for timer 2. */
    if(s->mode&0x100) return 0;
    if(s->counting<=0) cycles=0;
    uint32_t divided=(s->divider+cycles)>>3;
    s->divider=(s->divider+cycles)&7;
    if(s->mode&0x200) cycles=divided;
    if(s->mode&1) cycles=0;
    if((s->mode&8) && !s->target && !s->counter){
        (void)timer2_source_target(s,&pulses);return pulses;
    }
    if(!cycles) return 0;
    uint32_t before=s->counter;
    s->counter+=cycles;
    if(s->mode&0x40) s->irq_done=0;
    int exact=0;
    if((before<s->target && s->counter>=s->target) || s->counter>=s->target+65536u)
        exact=timer2_source_target(s,&pulses);
    if(s->counter>=65536u){
        s->mode|=0x1000;s->counter&=65535u;
        if((s->mode&0x20) && !s->irq_done){
            exact|=!s->counter;s->irq_done=1;pulses++;
        }
    }
    if((s->mode&0x40) && !exact) s->irq_done=0;
    return pulses;
}
static inline unsigned timer2_source_write(PsxTimer2Source *s,unsigned reg,uint16_t v){
    unsigned pulses=0;
    if(reg==0){s->counter=v;s->irq_done=0;}
    if(reg==4){
        s->mode=(s->mode&0x1c00)|(v&0x3ff);s->counter=0;s->irq_done=0;
        s->counting=1; /* Divider deliberately survives mode writes. */
    }
    if(reg==8) s->target=v;
    if(s->counter==s->target)(void)timer2_source_target(s,&pulses);
    return pulses;
}
static inline uint32_t timer2_source_read(PsxTimer2Source *s,unsigned reg){
    if(reg==0)return s->counter;
    if(reg==8)return s->target;
    if(reg!=4)return 0;
    uint32_t value=s->mode;s->mode&=~0x1000u;
    if(s->counter!=s->target)s->mode&=~0x800u;
    return value;
}
static inline uint32_t timer2_source_next(const PsxTimer2Source *s){
    /* Source periodic update bound, independent of CPU interrupt masking. */
    if(!(s->mode&0x30))return 1024;
    if((s->mode&8) && !s->counter && !s->target && !s->irq_done)return 1;
    if((s->mode&1) || s->counting<=0)return 1024;
    uint32_t target=((s->mode&0x18) && s->counter<s->target)?s->target:65536u;
    uint32_t clocks=target-s->counter;
    if(s->mode&0x200)clocks=clocks*8-s->divider;
    return clocks<1024?clocks:1024;
}
#endif
