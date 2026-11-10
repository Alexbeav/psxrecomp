#pragma once
#include <stdint.h>

/* Preserve the native reject at 4:3; in the wider render target retain the
 * axes proved meaningful by the game adapter. No guest flags are modified. */
static inline int psx_ws_masked_reject_value(uint32_t flags, uint32_t mask,
                                            int margin) {
    return (margin > 0 ? (flags & mask) : flags) != 0u;
}
