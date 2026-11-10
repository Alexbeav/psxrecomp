#pragma once
#include <cstdint>
#include <string>
#include <fmt/format.h>

/* Shared value semantics for the CFG and full-function emitters.
 * The caller supplies the existing decoder's GPR-write classification. */
inline std::string emit_load_value(uint32_t word, uint32_t writer,
                                   const std::string& code, bool begin = true) {
    const uint32_t op = word >> 26, rt = (word >> 16) & 31, rs = (word >> 21) & 31;
    const bool load = rt && ((op >= 0x20 && op <= 0x26) ||
        (op == 0x10 && rs == 0) || (op == 0x12 && (rs == 0 || rs == 2)));
    std::string out = begin ? "    psx_load_value_begin(cpu);\n" : "";
    if (load) {
        out += fmt::format("    {{ uint32_t psx_old_load_value = cpu->gpr[{}];\n", rt);
        out += code + "\n";
        out += fmt::format("    uint32_t psx_new_load_value = cpu->gpr[{0}];\n"
            "    cpu->gpr[{0}] = psx_old_load_value;\n"
            "    psx_load_value_arm(cpu, {0}u, psx_new_load_value); }}\n", rt);
    } else {
        out += code + "\n";
        if (writer) out += fmt::format("    psx_load_value_cancel(cpu, {}u);\n", writer);
    }
    return out;
}
