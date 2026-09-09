#ifndef PSX_SOURCE_OBSERVER_LIMIT_H
#define PSX_SOURCE_OBSERVER_LIMIT_H
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
#endif
