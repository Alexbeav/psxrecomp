/* CAUSE.IP2: the interrupt controller's single line into the CPU.
 *
 * PSX-SPX a253f078 docs/interrupts.md, "Interrupt Request / Execution": when
 * (I_STAT AND I_MASK) is nonzero, cop0r13.bit10 is set; the CPU takes the
 * interrupt only when SR also enables it. "PSX specific COP0 Notes": bit 10 is
 * not a latch; it clears as soon as (I_STAT AND I_MASK) is zero, and bits 11-15
 * are always zero. Bits 8-9 are the guest-written software-interrupt latches
 * (docs/cpuspecifications.md "cop0r13 - CAUSE"); they are never touched here.
 *
 * This only reflects the level: it charges no cycles and delivers nothing. */
#ifndef PSX_IRQ_CAUSE_IP2_H
#define PSX_IRQ_CAUSE_IP2_H

#include <stdint.h>

#define PSX_CAUSE_IP2 (1u << 10)

/* Set or clear bit 10 of *cause from the controller state; no-op without a
 * CAUSE register. */
static inline void psx_cause_ip2_apply(uint32_t *cause, uint32_t stat, uint32_t mask)
{
    if (!cause) return;
    if (stat & mask) *cause |= PSX_CAUSE_IP2;
    else *cause &= ~PSX_CAUSE_IP2;
}

#endif
