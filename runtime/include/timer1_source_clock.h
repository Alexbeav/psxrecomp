#ifndef PSX_TIMER1_SOURCE_CLOCK_H
#define PSX_TIMER1_SOURCE_CLOCK_H
#include <stdint.h>
#include <string.h>
/* Optional original-Octoshock timer 1 state machine. Independently expressed
 * from documented transitions; only IRQ-disabled modes are admitted. The
 * caller supplies ordered CPU, H-retrace and VBlank events. No guest memory. */
typedef struct PsxTimer1Source {
    uint32_t mode, counter, target;
    int counting, blank;
} PsxTimer1Source;
static inline void timer1_source_reset(PsxTimer1Source *s) {
    memset(s,0,sizeof(*s)); s->blank=1;
}
static inline void timer1_source_match(PsxTimer1Source *s) {
    s->mode|=0x800;
    if(s->mode&8) s->counter%=s->target ? s->target : 1;
}
static inline void timer1_source_count(PsxTimer1Source *s,uint32_t n) {
    if((s->mode&8) && !s->target && !s->counter) {
        timer1_source_match(s); return;
    }
    if(s->counting<=0 || !n) return;
    uint32_t before=s->counter;
    s->counter+=n;
    if((before<s->target && s->counter>=s->target) || s->counter>=s->target+65536u)
        timer1_source_match(s);
    if(s->counter>=65536u) {s->mode|=0x1000; s->counter&=65535u;}
}
static inline void timer1_source_cpu(PsxTimer1Source *s,uint32_t n) {
    if(!(s->mode&0x100)) timer1_source_count(s,n);
}
static inline void timer1_source_hblank(PsxTimer1Source *s,uint32_t n) {
    if(s->mode&0x100) timer1_source_count(s,n);
}
static inline void timer1_source_blank(PsxTimer1Source *s,int blank) {
    unsigned sync=s->mode&7;
    if(sync==1) s->counting=!blank;
    if(sync==5) s->counting=blank;
    if((sync==3 || sync==5) && s->blank && !blank) {
        s->counter=0;
        if(!s->target) timer1_source_match(s);
    }
    if(sync==7) {
        if(s->counting<0 && !s->blank && blank) s->counting=0;
        else if(!s->counting && s->blank && !blank) s->counting=1;
    }
    s->blank=blank;
}
static inline int timer1_source_write(PsxTimer1Source *s,unsigned reg,uint16_t v) {
    if(reg==4 && (v&0x30)) return 0; /* no IRQ phase claim */
    if(reg==0) s->counter=v;
    if(reg==4) {
        s->mode=(s->mode&0x1c00)|(v&0x3ff);s->counter=0;s->counting=1;
        if((v&7)==1) s->counting=!s->blank;
        if((v&7)==5) s->counting=s->blank;
        if((v&7)==7) s->counting=-1;
    }
    if(reg==8) s->target=v;
    if(s->counter==s->target) timer1_source_match(s);
    return 1;
}
static inline uint32_t timer1_source_read(PsxTimer1Source *s,unsigned reg) {
    if(reg==0) return s->counter;
    if(reg==8) return s->target;
    if(reg==4) {
        uint32_t value=s->mode;s->mode&=~0x1000u;
        if(s->counter!=s->target) s->mode&=~0x800u;
        return value;
    }
    return 0;
}
#endif
