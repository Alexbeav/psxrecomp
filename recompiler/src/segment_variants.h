#pragma once

// Per-segment compiled variants of static game code
// (docs/SEGMENT_AWARE_CODE.md §5.4).
//
// A PC carries a segment, and a compiled body bakes the segment it was
// compiled for into every link, EPC, fetch tag and store PC. The home compile
// is the image's link segment. A seed that names the image's bytes in another
// segment (a segment-qualified seed, §10 decision 3) asks for a variant: the
// home functions that seed reaches through direct edges (calls, tail jumps,
// split pieces, fallthroughs, CPS continuations), compiled again in that
// segment. Direct control flow never changes segment (§2), so that closure is
// everything the variant can reach without going through dispatch.
//
// A variant is compiled through a segment view of the image: the same bytes
// with the header moved into the variant's segment. Its code identity is then
// the variant VA (§5.1: func_A00100F0), the CodeGenerator's code segment is the
// view's, and every PC it bakes is in that segment. A KSEG1 variant is charged
// a fetch per instruction by #429's rule (§5.6) with no emitter change.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "code_generator.h"
#include "control_flow.h"
#include "function_analysis.h"
#include "ps1_exe_parser.h"

namespace PSXRecomp {

// The image seen from segment `seg`: the header's load address and entry move
// into `seg`, the bytes do not change. Addresses in the view are variant VAs.
PS1Executable segment_view(const PS1Executable& exe, uint32_t seg);

// One requested segment.
struct SegmentVariantPlan {
    uint32_t segment = 0;
    // The closure, rebased into `segment` and sorted by start address.
    std::vector<Function> functions;
    // The requested PCs, as written.
    std::vector<uint32_t> seeds;
    // Seeds that name no home dispatch entry (a function entry or a CPS
    // continuation of the home compile), or that lie in a segment that does
    // not map physical memory (maps_physical()), with the reason. Nothing is
    // compiled for them.
    std::vector<std::pair<uint32_t, std::string>> unplaced;
};

// The direct-edge successors of a home function that leave it: branch and
// jump targets and not-taken paths outside its blocks, call targets, call
// returns split into another piece, and a fall-through past its last block.
// Indirect transfers are not edges: they go through dispatch with the full PC.
std::vector<uint32_t> direct_successors(const ControlFlowGraph& cfg);

// Plans a variant per segment named by `seeds` (PCs outside the link segment
// that name the image's bytes). `home_functions` and `home_cfgs` are the home
// compile's final function set (CodeGenerator::last_functions / last_cfgs),
// and `home_continuations` its CPS continuation -> owner map. A seed at a home
// function entry roots that function; a seed at a CPS continuation roots its
// owner (the variant has the continuation too); an alias entry brings its
// whole alias group, which shares one body. Plans are in segment order.
std::vector<SegmentVariantPlan> plan_segment_variants(
    const PS1Executable& exe,
    const std::vector<Function>& home_functions,
    const std::map<uint32_t, ControlFlowGraph>& home_cfgs,
    const std::map<uint32_t, uint32_t>& home_continuations,
    const std::vector<uint32_t>& seeds);

// The codegen config for a variant in `seg`. Config code sites that the
// emitter matches exactly are written in the link segment (main_psx refuses
// any other spelling, §5.3); each one that names the image's bytes moves into
// `seg`, so the variant gets every hook and substitution the home body gets.
// Kinds matched by physical address need no move and are kept as written.
CodeGenConfig rebase_codegen_config(const CodeGenConfig& cfg,
                                    const PS1Executable& exe, uint32_t seg);

}  // namespace PSXRecomp
