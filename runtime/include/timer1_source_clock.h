#ifndef PSX_TIMER1_SOURCE_CLOCK_H
#define PSX_TIMER1_SOURCE_CLOCK_H
#include <stdint.h>

/* Independent compatibility implementation from PSX-SPX and authored public
 * register/raster/wire observations. See timer1_provenance.json for limits.
 * Counting and blank retain their persisted signed representations.
 */
typedef struct PsxTimer1Source {
    uint32_t mode, counter, target;
    int counting, blank;
} PsxTimer1Source;

static inline void timer1_source_reset(PsxTimer1Source *timer)
{
    *timer = (PsxTimer1Source){0, 0, 0, 0, 1};
}

static inline void timer1_source_equal(PsxTimer1Source *timer)
{
    if (timer->counter == timer->target) {
        timer->mode |= 2048u;
        if (timer->mode & 8u) timer->counter = 0;
    }
}

static inline void timer1_source_step(PsxTimer1Source *timer, uint32_t count)
{
    if (timer->counting <= 0) return;
    uint64_t start = timer->counter;
    uint64_t end = start + count;
    uint64_t distance = start < timer->target ? timer->target - start
                                            : 65536u - start + timer->target;
    int reached = count >= distance && !((timer->mode & 8u) && start == timer->target);
    if ((timer->mode & 8u) && (reached || start == timer->target))
        end = timer->target ? end % timer->target : 0;
    if (reached) timer->mode |= 2048u;
    if (end >= 65536u) timer->mode |= 4096u;
    timer->counter = (uint32_t)(end & 65535u);
}

static inline void timer1_source_cpu(PsxTimer1Source *timer, uint32_t n)
{
    if (!(timer->mode & 256u)) timer1_source_step(timer, n);
}

static inline void timer1_source_hblank(PsxTimer1Source *timer, uint32_t n)
{
    if (timer->mode & 256u) timer1_source_step(timer, n);
}

static inline void timer1_source_blank(PsxTimer1Source *timer, int blank)
{
    int previous = timer->blank;
    timer->blank = !!blank;
    if (!(timer->mode & 1u)) return;
    switch ((timer->mode >> 1) & 3u) {
    case 0: timer->counting = !timer->blank; break;
    case 1:
    case 2:
        if (previous && !timer->blank) {
            timer->counter = 0;
            timer1_source_equal(timer);
        }
        timer->counting = (timer->mode & 4u) ? timer->blank : 1;
        break;
    case 3:
        if (timer->counting < 0 && timer->blank) timer->counting = 0;
        else if (timer->counting == 0 && previous && !timer->blank) timer->counting = 1;
        break;
    }
}

static inline int timer1_source_write(PsxTimer1Source *timer, unsigned reg, uint16_t value)
{
    if (reg == 0) timer->counter = value;
    else if (reg == 8) timer->target = value;
    else if (reg == 4) {
        if (value & 48u) return 0;
        timer->mode = (timer->mode & 6144u) | (value & 1023u);
        timer->counter = 0;
        if (!(value & 1u)) timer->counting = 1;
        else switch ((value >> 1) & 3u) {
        case 0: timer->counting = !timer->blank; break;
        case 1: timer->counting = 1; break;
        case 2: timer->counting = timer->blank; break;
        case 3: timer->counting = -1; break;
        }
    } else return 0;
    timer1_source_equal(timer);
    return 1;
}

static inline uint32_t timer1_source_read(PsxTimer1Source *timer, unsigned reg)
{
    if (reg == 0) return timer->counter;
    if (reg == 8) return timer->target;
    if (reg != 4) return 0;
    uint32_t result = timer->mode;
    timer->mode &= ~UINT32_C(6144);
    if (timer->counter == timer->target) timer->mode |= result & 2048u;
    return result;
}
#endif
