#ifndef PSX_TIMER2_SOURCE_CLOCK_H
#define PSX_TIMER2_SOURCE_CLOCK_H
#include <stdint.h>

/* Independent compatibility implementation from hardware documentation and
 * authored timer register experiments. This profile differs from silicon;
 * see runtime/tests/timer2_provenance.json. Old implementations remain in Git.
 */
typedef struct PsxTimer2Source {
    uint32_t counter, mode, target, divider;
    int irq_done, counting;
} PsxTimer2Source;

static inline void timer2_source_reset(PsxTimer2Source *timer)
{
    *timer = (PsxTimer2Source){0, 0, 0, 0, 0, 0};
}

static inline unsigned timer2_source_pulse(PsxTimer2Source *timer, int requested)
{
    if (!requested || (timer->irq_done && !(timer->mode & 64u)))
        return 0;
    timer->irq_done = 1;
    return 1;
}

static inline unsigned timer2_source_cpu(PsxTimer2Source *timer, uint32_t cycles)
{
    uint64_t divided = (uint64_t)timer->divider + cycles;
    uint64_t steps = timer->mode & 512u ? divided / 8u : cycles;
    timer->divider = (uint32_t)(divided & 7u);
    if (!timer->counting || (timer->mode & 256u))
        return 0;

    uint64_t start = timer->counter;
    uint64_t end = start + steps;
    int overflow = 0, target_hit = 0;
    if ((timer->mode & 8u) && start <= timer->target) {
        target_hit = start < timer->target && end >= timer->target;
        timer->counter = timer->target ? (uint32_t)(end % timer->target) : 0;
    } else {
        overflow = end >= 65536u;
        uint64_t distance = start < timer->target ? timer->target - start
                                                : 65536u - start + timer->target;
        target_hit = steps >= distance;
        uint32_t wrapped = (uint32_t)(end & 65535u);
        if ((timer->mode & 8u) && overflow)
            timer->counter = timer->target ? wrapped % timer->target : 0;
        else
            timer->counter = wrapped;
    }
    if (target_hit) timer->mode |= 2048u;
    if (overflow) timer->mode |= 4096u;
    return timer2_source_pulse(timer, (target_hit && (timer->mode & 16u)) ||
                                     (overflow && (timer->mode & 32u)));
}

static inline unsigned timer2_source_write(PsxTimer2Source *timer, unsigned reg, uint16_t value)
{
    if (reg == 0) {
        timer->counter = value;
        timer->irq_done = 0;
    } else if (reg == 4) {
        timer->mode = value & 1023u;
        timer->counter = 0;
        timer->irq_done = 0;
        timer->counting = !(value & 1u);
    } else if (reg == 8) {
        timer->target = value;
    } else {
        return 0;
    }
    if (timer->counter != timer->target)
        return 0;
    timer->mode |= 2048u;
    if (timer->mode & 8u)
        timer->counter = 0;
    return timer2_source_pulse(timer, !!(timer->mode & 16u));
}

static inline uint32_t timer2_source_read(PsxTimer2Source *timer, unsigned reg)
{
    if (reg == 0) return timer->counter;
    if (reg == 8) return timer->target;
    if (reg != 4) return 0;
    uint32_t result = timer->mode;
    timer->mode &= ~UINT32_C(6144);
    if (timer->counter == timer->target)
        timer->mode |= result & 2048u;
    return result;
}

static inline uint32_t timer2_source_next(const PsxTimer2Source *timer)
{
    if (!timer->counting)
        return 1024;
    uint64_t distance = UINT64_C(1) << 32;
    if ((timer->mode & 16u) || ((timer->mode & 40u) == 40u))
        distance = timer->counter < timer->target ? timer->target - timer->counter
                                                 : 65536u - timer->counter + timer->target;
    if ((timer->mode & 32u) && 65536u - timer->counter < distance)
        distance = 65536u - timer->counter;
    if (timer->mode & 512u)
        distance = distance * 8u - timer->divider;
    return distance < 1024 ? (uint32_t)distance : 1024;
}
#endif
