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

#include "code_generator.h"
#include "ps1_exe_parser.h"

namespace PSXRecomp {

// Emits the dispatch source for `dispatch_addrs` (compile addresses of the
// dispatchable func_ entries) and the generator's CPS continuations. Row keys
// and resume PCs go through codegen.runtime_pc() (§5.2); func_ names keep the
// compile identity. `ranges_manifest` is the emitted .ranges text, whose F/R
// records give each row its exact instruction ranges.
//
// Returns false with `error` set when two rows would share a physical word:
// one compile has one code segment, so that can only be a caller error until
// per-segment variants (§5.4) share the table.
bool emit_game_dispatch(const CodeGenerator& codegen,
                        const PS1Executable& exe,
                        const std::set<uint32_t>& dispatch_addrs,
                        const std::string& ranges_manifest,
                        std::string& out,
                        std::string& error);

}  // namespace PSXRecomp
