/* PS1B-187: the CAUSE.IP2 rule that interrupts.c applies
 * (psx_irq_refresh_cause_ip2 / psx_irq_set_cause_ptr call psx_cause_ip2_apply).
 *
 * PSX-SPX a253f078 docs/interrupts.md:
 *   "Interrupt Request / Execution": (I_STAT AND I_MASK) nonzero sets
 *   cop0r13.bit10;
 *   "PSX specific COP0 Notes": bit 10 is not a latch, it clears as soon as
 *   (I_STAT AND I_MASK) is zero; bits 8-9 are software latches;
 *   "Interrupt Acknowledge": the guest clears an I_STAT bit by writing 0.
 * Cases: spec rules 2-4. */
#include "irq_cause_ip2.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

int main(void)
{
    /* Rule 4: no CAUSE register is a safe no-op. */
    psx_cause_ip2_apply(NULL, 1u, 1u);
    check(1, "null CAUSE pointer does not crash");

    uint32_t cause = 0x00000300u;            /* software bits 8-9 set by the guest */
    psx_cause_ip2_apply(&cause, 0, 0);
    check(cause == 0x00000300u, "idle controller: bit 10 clear, bits 8-9 kept");

    /* Rule 2: a masked source becoming pending sets bit 10. */
    psx_cause_ip2_apply(&cause, 1u << 3, 1u << 3);
    check(cause == (0x00000300u | (1u << 10)), "pending and enabled: bit 10 set");

    /* Pending but not enabled: bit 10 clear. */
    psx_cause_ip2_apply(&cause, 1u << 2, 1u << 3);
    check(!(cause & (1u << 10)), "pending but masked: bit 10 clear");

    /* Rule 2: acknowledging (writing 0 to the I_STAT bit) clears bit 10. */
    psx_cause_ip2_apply(&cause, 1u << 3, 1u << 3);
    check(cause & (1u << 10), "set again");
    psx_cause_ip2_apply(&cause, 0, 1u << 3);
    check(!(cause & (1u << 10)), "acknowledge clears bit 10 (not a latch)");

    /* Rule 2: masking a pending source clears bit 10. */
    psx_cause_ip2_apply(&cause, 1u << 3, 1u << 3);
    psx_cause_ip2_apply(&cause, 1u << 3, 0);
    check(!(cause & (1u << 10)), "masking a pending source clears bit 10");

    /* Rule 3: only bit 10 changes; every other bit is preserved. */
    cause = 0xFFFFFBFFu;
    psx_cause_ip2_apply(&cause, 1u, 1u);
    check(cause == 0xFFFFFFFFu, "set touches only bit 10");
    psx_cause_ip2_apply(&cause, 0, 1u);
    check(cause == 0xFFFFFBFFu, "clear touches only bit 10");

    printf("irq cause ip2 (PSX-SPX): %u checks passed\n", checks);
    return 0;
}
