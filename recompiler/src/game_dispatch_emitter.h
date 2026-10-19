#pragma once

// The game dispatch table (<exe>_dispatch.c): every dispatchable compiled
// entry and CPS continuation of a PS-X EXE, keyed by the PC the guest
// executes, plus the lookup, validity and dispatch functions the runtime
// calls (psx_game_find_entry, psx_game_text_native_ok[_full],
// psx_dispatch_game_compiled, psx_game_is_function_entry).
//
// Code identity is the full virtual address (docs/SEGMENT_AWARE_CODE.md §5.1,
// §5.5): a row answers only for the exact PC it was compiled for. The lookup
// indexes rows by physical word and then requires row.addr == addr, so a PC in
// another segment at the same word misses. psx_game_address_in_text stays
// physical (byte identity), which sends such a segment miss down the runtime's
// clean-text-miss path: it is interpreted and recorded, never run through
// another segment's body.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "code_generator.h"
#include "ps1_exe_parser.h"

namespace PSXRecomp {

// One compile whose bodies the table dispatches: the home compile, or a
// segment variant (§5.4). `dispatch_addrs` are the compile addresses of its
// dispatchable func_ entries; its CPS continuations come from the generator.
struct GameDispatchUnit {
    const CodeGenerator* codegen;
    std::set<uint32_t> dispatch_addrs;
};

// Emits the dispatch source for every unit's entries and CPS continuations.
// Row keys and resume PCs go through the unit's runtime_pc() (§5.2); func_
// names keep the compile identity. `ranges_manifest` is the emitted .ranges
// text of all units, whose F/R records give each row its exact instruction
// ranges. Rows are sorted by physical word; one word may carry a row per
// compiled segment (the home body and its variants), and the lookup requires
// the exact PC. The first unit's CPS mode applies to the table.
//
// Returns false with `error` set when two rows name the same PC. Within one
// compile that means two rows on one physical word: one compile has one code
// segment.
bool emit_game_dispatch(const std::vector<GameDispatchUnit>& units,
                        const PS1Executable& exe,
                        const std::string& ranges_manifest,
                        std::string& out,
                        std::string& error);

// The single-compile form (no segment variants).
bool emit_game_dispatch(const CodeGenerator& codegen,
                        const PS1Executable& exe,
                        const std::set<uint32_t>& dispatch_addrs,
                        const std::string& ranges_manifest,
                        std::string& out,
                        std::string& error);

}  // namespace PSXRecomp
