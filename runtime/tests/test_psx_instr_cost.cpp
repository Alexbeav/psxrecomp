#include "psx_instr_cost.h"

#include <cstdint>
#include <cstdio>

static uint32_t reference_dep_res_mask(uint32_t insn) {
    const uint32_t op = insn >> 26;
    const uint32_t rs = (insn >> 21) & 0x1Fu;
    const uint32_t rt = (insn >> 16) & 0x1Fu;
    const uint32_t rd = (insn >> 11) & 0x1Fu;
    const uint32_t m_rs = 1u << rs;
    const uint32_t m_rt = 1u << rt;
    const uint32_t m_rd = 1u << rd;
    switch (op) {
    case 0x00:
        switch (insn & 0x3Fu) {
        case 0x00: case 0x02: case 0x03: return m_rt | m_rd;
        case 0x04: case 0x06: case 0x07: return m_rs | m_rt | m_rd;
        case 0x08: case 0x09: return m_rs | m_rd;
        case 0x10: case 0x12: return m_rd;
        case 0x11: case 0x13: return m_rs;
        case 0x18: case 0x19: case 0x1A: case 0x1B: return m_rs | m_rt;
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x2A: case 0x2B: return m_rs | m_rt | m_rd;
        default: return 0u;
        }
    case 0x01:
        return m_rs | (((rt & 0x1Eu) == 0x10u) ? (1u << 31) : 0u);
    case 0x02: return 0u;
    case 0x03: return 1u << 31;
    case 0x04: case 0x05: return m_rs | m_rt;
    case 0x06: case 0x07: return m_rs;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: return m_rs | m_rt;
    case 0x0F: return m_rt;
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: return m_rs;
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2E: return m_rs | m_rt;
    default: return 0u;
    }
}

/* Native-span charges against the reference main-RAM load interlock
 * (psx_cyc.h: issue 1, ReadFudge 2, wait 3, completion 2; the LDAbsorb
 * give-back is spent by later independent instructions). A flat +2 per load
 * made a kernel TestEvent poll about a third cheaper than the reference, so a
 * game's pass-count timeout on a memory-card read expired before the read
 * finished (PS1B-695 Tiny Toon; the RE3 Europe LOAD "Access error"). */
static int check_native_span_costs() {
    int failures = 0;
    const uint32_t lw = 0x8C580004u;  /* lw t8,4(v0) */
    if (psx_native_instr_cycles(lw, 0x80001000u) != 8u) {
        std::fprintf(stderr, "cached main-RAM lw: expected 8, got %u\n",
                     psx_native_instr_cycles(lw, 0x80001000u));
        ++failures;
    }
    if (psx_native_instr_cycles(lw, 0xBFC01000u) != 12u) {
        std::fprintf(stderr, "uncached main-RAM lw: expected 12, got %u\n",
                     psx_native_instr_cycles(lw, 0xBFC01000u));
        ++failures;
    }
    /* The B0 TestEvent body a card-wait loop polls: two dependent loads. */
    static const uint32_t poll[] = {
        0x3C0E8000u, /* lui t6,0x8000 */
        0x8DCE0040u, /* lw t6,64(t6) */
        0x01E47823u, /* subu t7,t7,a0 */
        0x000F7880u, /* sll t7,t7,2 */
        0x01CF1021u, /* addu v0,t6,t7 */
        0x8C580004u, /* lw t8,4(v0) */
        0x24014000u, /* addiu at,zero,0x4000 */
        0x03011026u, /* xor v0,t8,at */
        0x03E00008u, /* jr ra */
        0x00000000u, /* nop */
    };
    PsxNativeSpanModel m;
    psx_native_span_model_init(&m);
    uint32_t total = 0u;
    for (unsigned i = 0; i < sizeof(poll) / sizeof(poll[0]); ++i) {
        total += psx_native_span_model_step(&m, poll[i], 0x80000500u + 4u * i);
    }
    if (total != 21u) {
        std::fprintf(stderr, "TestEvent poll span: expected 21 (reference), got %u "
                     "(flat +2 per load gives 14)\n", total);
        ++failures;
    }
    return failures;
}

int main() {
    if (check_native_span_costs() != 0) return 1;
    for (uint32_t op = 0; op < 64u; ++op) {
        const uint32_t function_count = (op == 0u) ? 64u : 1u;
        for (uint32_t fn = 0; fn < function_count; ++fn) {
            for (uint32_t rs = 0; rs < 32u; ++rs) {
                for (uint32_t rt = 0; rt < 32u; ++rt) {
                    for (uint32_t rd = 0; rd < 32u; ++rd) {
                        const uint32_t insn = (op << 26) | (rs << 21) |
                                              (rt << 16) | (rd << 11) | fn;
                        const uint32_t expected = reference_dep_res_mask(insn);
                        const uint32_t actual = psx_cyc_dep_res_mask(insn);
                        if (actual != expected) {
                            std::fprintf(stderr,
                                         "dep/res mismatch insn=%08x expected=%08x actual=%08x\n",
                                         insn, expected, actual);
                            return 1;
                        }
                    }
                }
            }
        }
    }
    std::puts("PASS: native-span load interlock matches the reference; "
              "table-driven dependency masks match the frozen decoder");
    return 0;
}
