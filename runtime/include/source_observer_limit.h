#ifndef PSX_SOURCE_OBSERVER_LIMIT_H
#define PSX_SOURCE_OBSERVER_LIMIT_H
#include <stdio.h>
#include <stdlib.h>
/* A declared route may exceed the historical short-TAS diagnostic bound.
 * Reject signs, whitespace, overflow and unbounded collection. */
static inline unsigned source_observer_max_frames(const char *variable) {
    const char *limit=getenv(variable);
    if(!limit)return 20000;
    unsigned value=0;
    if(!*limit)abort();
    for(const char *p=limit;*p;++p) {
        if(*p<'0'||*p>'9'||value>100000)abort();
        value=value*10+(unsigned)(*p-'0');
        if(value>1000000)abort();
    }
    if(!value)abort();
    return value;
}
/* The route ends at the first VBlank sample after its last input, and the
 * observers count source GPU frame returns: two clocks. Whether the return
 * that follows the declared last return comes before that sample depends on
 * their phase (Discworld II 3673 returns once more; the other 10k routes do
 * not). That one return is not recorded, so the capture stays exactly the
 * declared returns. A second one past the cap is a runaway and aborts. */
static inline int source_observer_frame_recorded(const char *probe,unsigned frame,unsigned max_frames) {
    if(frame<1 || frame>max_frames+1u)abort();
    if(frame>max_frames) {
        fprintf(stderr,"%s: return %u is past the cap %u; not recorded\n",probe,frame,max_frames);
        return 0;
    }
    return 1;
}
#endif
