/* I_STAT / I_MASK register image rules (PS1B-214, [ORACLE FIXTURE R1]). */
#include "irq_register_image.h"
#include <assert.h>
#include <stdio.h>

static uint32_t mask_write(uint32_t stored, uint32_t val, uint32_t lanes, int source)
{
    return ((stored & ~lanes) | (val & lanes)) & irq_mask_store_bits(source);
}

int main(void)
{
    /* Source profile: writing FFFFFFFFh to I_MASK reads back 1F80FFFFh (word),
     * FFFFh (low halfword) and 1F80h (high halfword). */
    uint32_t m = mask_write(0, 0xFFFFFFFFu, 0xFFFFFFFFu, 1);
    uint32_t word = irq_register_read_image(m, 1);
    assert(word == 0x1F80FFFFu);
    assert((word & 0xFFFFu) == 0xFFFFu);
    assert((word >> 16) == 0x1F80u);
    /* A halfword write reaches bits 11-15 in the source profile too. */
    assert(mask_write(0, 0xF800u, 0xFFFFu, 1) == 0xF800u);
    /* I_STAT reads carry the same upper halfword: all-zero status reads 1F800000h. */
    assert(irq_register_read_image(0, 1) == 0x1F800000u);

    /* Default runtime: bits 11-15 are not used and read as zero, no upper bits. */
    m = mask_write(0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    assert(m == 0x7FFu);
    assert(irq_register_read_image(m, 0) == 0x7FFu);
    assert(mask_write(0, 0xF800u, 0xFFFFu, 0) == 0u);

    /* IRQ decisions only ever see bits 0-10 of the stored mask. */
    assert((mask_write(0, 0xFFFFFFFFu, 0xFFFFFFFFu, 1) & IRQ_REG_DEFAULT_BITS) == 0x7FFu);
    puts("irq_register_image: PASS");
    return 0;
}
