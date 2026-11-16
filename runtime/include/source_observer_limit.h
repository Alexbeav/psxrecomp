#ifndef PSX_SOURCE_OBSERVER_LIMIT_H
#define PSX_SOURCE_OBSERVER_LIMIT_H
#include <stdlib.h>
#include <string.h>
/* A declared route may exceed the historical short-TAS diagnostic bound.
 * Reject signs, whitespace, overflow and unbounded collection. */
static inline unsigned source_observer_max_frames(const char *variable) {
    const char *limit=getenv(variable);
    if(!limit)return 20000;
    const char *capacity=getenv("PSX_SOURCE_OBSERVER_CAPACITY_V1");
    const unsigned maximum=capacity && !strcmp(capacity,"1") ? 4000000u : 1000000u;
    unsigned value=0;
    if(!*limit)abort();
    for(const char *p=limit;*p;++p) {
        if(*p<'0'||*p>'9'||value>(maximum-(unsigned)(*p-'0'))/10u)abort();
        value=value*10+(unsigned)(*p-'0');
        if(value>maximum)abort();
    }
    if(!value)abort();
    return value;
}
#endif
