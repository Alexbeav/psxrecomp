#ifndef PSX_INSTR_COST_H
#define PSX_INSTR_COST_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Independently authored from the pure-opcode T172 observation matrices.
 * This issue cost excludes the separately accounted memory/device waits. */
static inline uint32_t psx_instr_base_cycles(uint32_t insn)
{
    (void)insn;
    return 1;
}

static inline uint32_t psx_cyc_dep_res_mask(uint32_t insn)
{
    const unsigned primary = insn >> 26;
    const unsigned rt = (insn >> 16) & 31u;
    const uint32_t source = UINT32_C(1) << ((insn >> 21) & 31u);
    const uint32_t target = UINT32_C(1) << rt;
    const uint32_t destination = UINT32_C(1) << ((insn >> 11) & 31u);
    switch (primary) {
    case 0:
        switch (insn & 63u) {
        case 0: case 2: case 3:
            return target | destination;
        case 4: case 6: case 7:
        case 32: case 33: case 34: case 35:
        case 36: case 37: case 38: case 39: case 42: case 43:
            return source | target | destination;
        case 8: case 9:
            return source | destination;
        case 16: case 18:
            return destination;
        case 17: case 19:
            return source;
        case 24: case 25: case 26: case 27:
            return source | target;
        default:
            return 0;
        }
    case 1:
        return source | ((rt == 16 || rt == 17) ? UINT32_C(0x80000000) : 0);
    case 3:
        return UINT32_C(0x80000000);
    case 4: case 5:
    case 8: case 9: case 10: case 11: case 12: case 13: case 14:
    case 40: case 41: case 42: case 43: case 46:
        return source | target;
    case 6: case 7:
    case 32: case 33: case 34: case 35: case 36: case 37: case 38:
        return source;
    case 15:
        return target;
    default:
        return 0;
    }
}
#ifdef __cplusplus
}
#endif
#endif
