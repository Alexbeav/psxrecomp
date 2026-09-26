#ifndef PSX_IRQ_REGISTER_IMAGE_H
#define PSX_IRQ_REGISTER_IMAGE_H

#include <stdint.h>

/* I_STAT (1F801070h) and I_MASK (1F801074h) register rules, kept pure so the
 * unit test can check them without the memory map.
 *
 * Default runtime: only bits 0-10 exist (PSX-SPX "1F801070h I_STAT" /
 * "1F801074h I_MASK": bits 11-15 not used, always zero), so a mask write keeps
 * 0x7FF and reads return the stored value.
 *
 * Source (comparison) profile, [ORACLE FIXTURE R1]:
 *   - I_MASK stores and reads back bits 0-15: writing FFFFFFFFh reads back
 *     1F80FFFFh as a word, FFFFh from the low halfword;
 *   - both registers read the fixed upper halfword 1F80h.
 * IRQ delivery and CAUSE.IP2 keep using bits 0-10 only; I_STAT never holds
 * bits 11-15, so the wider stored mask cannot raise an interrupt. */

#define IRQ_REG_DEFAULT_BITS 0x07FFu
#define IRQ_REG_SOURCE_MASK_BITS 0xFFFFu
#define IRQ_REG_SOURCE_UPPER 0x1F800000u

static inline uint32_t irq_mask_store_bits(int source_profile)
{
    return source_profile ? IRQ_REG_SOURCE_MASK_BITS : IRQ_REG_DEFAULT_BITS;
}

static inline uint32_t irq_register_read_image(uint32_t value, int source_profile)
{
    return value | (source_profile ? IRQ_REG_SOURCE_UPPER : 0u);
}

#endif
