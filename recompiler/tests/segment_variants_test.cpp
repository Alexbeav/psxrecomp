// Segment variants of static game code (docs/SEGMENT_AWARE_CODE.md §5.4,
// rollout PR D): the planner that turns segment-qualified seeds into per-
// segment direct-edge closures, the segment view of an image, and the codegen
// config a variant compiles with.
//
// The planner works on a hand-built KSEG0 image whose functions are given
// explicitly (so the test does not depend on discovery heuristics):
//   F0  0x00  jal F1 / j F2           a call and a tail jump
//   U   0x10  jr $ra                  reached by nothing
//   F1  0x18  jr $ra                  F0's callee
//   F2  0x20  beq -> F4, then runs    a split branch out of the function and a
//             off its end into F3     fall-through past its last block
//   F3  0x2C  jr $ra
//   F4  0x38  jr $ra
//   H   0x40  host with alias entries A1 0x44 and A2 0x48 (one shared body)
//   F5  0x50  beq -> F1 as its last pair: taken and not taken both leave
//   F6  0x58  jr $ra                  F5's not-taken path
// It checks:
//   - a seed at a function entry roots that function; its closure follows
//     calls, tail jumps, out-of-function branch targets and fall-throughs,
//     and nothing else (U and H stay out);
//   - a seed at a CPS continuation roots its owner;
//   - an alias entry brings its whole alias group, not its host;
//   - a seed that names no home dispatch entry is reported, not guessed, and
//     so is a seed in a segment that does not map physical memory
//     (0x20000000-0x7FFFFFFF, KSEG2);
//   - plans come out per segment, in segment order, rebased: start/end,
//     name, alias host, alias group and producer range all move into the
//     segment;
//   - direct_successors lists exactly the edges that leave a function.
// segment_view moves the header and keeps the bytes, and
// rebase_codegen_config moves the exact-match code sites that name the image
// into the variant's segment, keeps other addresses and the physically
// matched kinds as written, and turns the split pre-pass off.
// overlay_codegen_config (an overlay capture view, §5.7) moves every
// exact-match site whose bytes lie in the image, in any segment that maps RAM,
// into the view's segment, and keeps everything else, the split pre-pass
// setting included.

#include "segment_variants.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

constexpr uint32_t B = 0x80010000u;
constexpr uint32_t KUSEG = 0x00000000u, KSEG1 = 0xA0000000u;

uint32_t jal(uint32_t t) { return 0x0C000000u | ((t >> 2) & 0x03FFFFFFu); }
uint32_t j(uint32_t t) { return 0x08000000u | ((t >> 2) & 0x03FFFFFFu); }
uint32_t beqz(uint32_t pc, uint32_t t) {  // beq a0, zero, t
    return 0x10800000u | (((t - (pc + 4u)) >> 2) & 0xFFFFu);
}
constexpr uint32_t JR_RA = 0x03E00008u, NOP = 0u, ADDIU_V0 = 0x24020001u;

PSXRecomp::PS1Executable image() {
    std::vector<uint32_t> w(0x60 / 4, NOP);
    auto at = [&](uint32_t off, uint32_t word) { w[off / 4] = word; };
    at(0x00, jal(B + 0x18)); at(0x08, j(B + 0x20));        // F0
    at(0x10, JR_RA);                                        // U
    at(0x18, JR_RA);                                        // F1
    at(0x20, beqz(B + 0x20, B + 0x38)); at(0x28, ADDIU_V0); // F2
    at(0x2C, JR_RA);                                        // F3
    at(0x38, JR_RA);                                        // F4
    at(0x40, ADDIU_V0); at(0x44, ADDIU_V0); at(0x48, JR_RA);  // H, A1, A2
    at(0x50, beqz(B + 0x50, B + 0x18));                        // F5
    at(0x58, JR_RA);                                            // F6
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = B;
    exe.header.initial_pc = B;
    exe.header.file_size = static_cast<uint32_t>(w.size() * 4u);
    for (uint32_t word : w)
        for (int i = 0; i < 4; ++i) exe.code_data.push_back(static_cast<uint8_t>(word >> (8 * i)));
    return exe;
}

PSXRecomp::Function fn(uint32_t lo, uint32_t hi, uint32_t alias_host = 0,
                       std::vector<uint32_t> group = {}) {
    PSXRecomp::Function f;
    f.start_addr = lo;
    f.end_addr = hi;
    f.size = hi - lo;
    f.has_prologue = f.has_epilogue = false;
    f.stack_frame_size = 0;
    f.name = "func_" + std::to_string(lo);
    f.alias_walk_lo = alias_host;
    f.alias_group_entries = std::move(group);
    return f;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

std::set<uint32_t> starts(const PSXRecomp::SegmentVariantPlan& p) {
    std::set<uint32_t> out;
    for (const auto& f : p.functions) out.insert(f.start_addr);
    return out;
}

}  // namespace

int main() {
    const auto exe = image();
    const std::vector<uint32_t> group{B + 0x44, B + 0x48};
    const std::vector<PSXRecomp::Function> home{
        fn(B + 0x00, B + 0x10), fn(B + 0x10, B + 0x18), fn(B + 0x18, B + 0x20),
        fn(B + 0x20, B + 0x2C), fn(B + 0x2C, B + 0x34), fn(B + 0x38, B + 0x40),
        fn(B + 0x40, B + 0x50), fn(B + 0x44, B + 0x50, B + 0x40, group),
        fn(B + 0x48, B + 0x50, B + 0x40, group), fn(B + 0x50, B + 0x58),
        fn(B + 0x58, B + 0x60)};
    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfgs = analyzer.analyze_all_functions(home);

    // direct_successors: the edges that leave a function, and only those.
    {
        const auto f0 = PSXRecomp::direct_successors(cfgs.at(B));
        check(f0 == std::vector<uint32_t>{B + 0x18, B + 0x20},
              "F0 leaves by its jal and its j, got " + std::to_string(f0.size()));
        const auto f2 = PSXRecomp::direct_successors(cfgs.at(B + 0x20));
        check(f2 == std::vector<uint32_t>{B + 0x2C, B + 0x38},
              "F2 leaves by its branch target and by running off its end");
        check(PSXRecomp::direct_successors(cfgs.at(B + 0x18)).empty(),
              "a jr $ra is no direct edge");
        check(PSXRecomp::direct_successors(cfgs.at(B + 0x50)) ==
                  std::vector<uint32_t>{B + 0x18, B + 0x58},
              "F5's final branch leaves both taken and not taken");
    }

    const std::map<uint32_t, uint32_t> conts{{B + 0x08, B}};
    const std::vector<uint32_t> seeds{KSEG1 | B, KUSEG | ((B + 0x44) & 0x1FFFFFFFu),
                                      KSEG1 | ((B + 0x08) & 0x1FFFFFFFu),
                                      KSEG1 | ((B + 0x04) & 0x1FFFFFFFu)};
    const auto plans = PSXRecomp::plan_segment_variants(exe, home, cfgs, conts, seeds);
    check(plans.size() == 2 && plans[0].segment == KUSEG && plans[1].segment == KSEG1,
          "one plan per requested segment, in segment order");
    if (plans.size() == 2) {
        const auto& ku = plans[0];
        const auto& k1 = plans[1];
        const uint32_t p = B & 0x1FFFFFFFu;
        check(starts(ku) == std::set<uint32_t>{p + 0x44, p + 0x48},
              "an alias entry brings its alias group, not its host");
        check(starts(k1) == std::set<uint32_t>{KSEG1 | p, KSEG1 | (p + 0x18), KSEG1 | (p + 0x20),
                                               KSEG1 | (p + 0x2C), KSEG1 | (p + 0x38)},
              "the KSEG1 closure is F0 with its call, tail jump, branch target and "
              "fall-through, and neither U nor H");
        check(k1.seeds.size() == 3 && ku.seeds.size() == 1, "every seed is listed with its plan");
        check(k1.unplaced.size() == 1 && k1.unplaced[0].first == (KSEG1 | (p + 4)) &&
                  k1.unplaced[0].second.find(hex(B + 4)) != std::string::npos,
              "a seed inside a function body names no dispatch entry and is reported "
              "with its home PC");
        bool sorted = true, rebased = true;
        for (size_t i = 0; i < k1.functions.size(); ++i) {
            const auto& f = k1.functions[i];
            if (i && k1.functions[i - 1].start_addr >= f.start_addr) sorted = false;
            if ((f.end_addr & 0xE0000000u) != KSEG1 || f.name != "func_" + hex(f.start_addr))
                rebased = false;
        }
        check(sorted && rebased, "KSEG1 functions are sorted, with rebased ends and names");
        for (const auto& f : ku.functions) {
            check(f.alias_walk_lo == p + 0x40 &&
                      f.alias_group_entries == std::vector<uint32_t>{p + 0x44, p + 0x48} &&
                      f.name == "func_" + hex(f.start_addr),
                  "alias host and group move into the variant's segment");
        }
    }
    // A branch whose not-taken path runs past the function brings the next.
    {
        const auto f5 = PSXRecomp::plan_segment_variants(
            exe, home, cfgs, conts, {KSEG1 | ((B + 0x50) & 0x1FFFFFFFu)});
        const uint32_t p = B & 0x1FFFFFFFu;
        check(f5.size() == 1 &&
                  starts(f5[0]) == std::set<uint32_t>{KSEG1 | (p + 0x18), KSEG1 | (p + 0x50),
                                                      KSEG1 | (p + 0x58)},
              "F5's closure is F1 (taken) and F6 (not taken)");
    }
    // Only KUSEG below 0x20000000, KSEG0 and KSEG1 map physical memory: a seed
    // in 0x20000000-0x7FFFFFFF or in KSEG2 is no alias of the image's bytes.
    // It is reported with its home PC and compiles nothing.
    {
        const uint32_t p = B & 0x1FFFFFFFu;
        const auto bad = PSXRecomp::plan_segment_variants(
            exe, home, cfgs, conts, {0x20000000u | p, 0xC0000000u | p, 0x60000000u | p});
        bool refused = bad.size() == 3;
        for (const auto& plan : bad) {
            refused = refused && plan.functions.empty() && plan.unplaced.size() == 1 &&
                      plan.unplaced[0].first == (plan.segment | p) &&
                      plan.unplaced[0].second.find("does not map physical memory") !=
                          std::string::npos &&
                      plan.unplaced[0].second.find(hex(B)) != std::string::npos;
        }
        check(refused, "a seed in a segment that does not map physical memory is "
                       "reported with its home PC and compiles nothing");
        check(PSXRecomp::maps_physical(0x1FFFFFFCu) && !PSXRecomp::maps_physical(0x20000000u) &&
                  !PSXRecomp::maps_physical(0x7FFFFFFCu) && PSXRecomp::maps_physical(0x80000000u) &&
                  PSXRecomp::maps_physical(0xBFFFFFFCu) && !PSXRecomp::maps_physical(0xC0000000u) &&
                  !PSXRecomp::maps_physical(0xFFFE0130u),
              "maps_physical is KUSEG below 0x20000000, KSEG0 and KSEG1");
    }
    // A seed at a CPS continuation alone roots its owner.
    {
        const auto only = PSXRecomp::plan_segment_variants(
            exe, home, cfgs, conts, {KSEG1 | ((B + 0x08) & 0x1FFFFFFFu)});
        check(only.size() == 1 && only[0].unplaced.empty() &&
                  starts(only[0]).count(KSEG1 | (B & 0x1FFFFFFFu)),
              "a continuation seed roots the function that owns it");
    }

    // segment_view: the header moves, the bytes stay.
    {
        const auto view = PSXRecomp::segment_view(exe, KSEG1);
        check(view.load_address() == (KSEG1 | (B & 0x1FFFFFFFu)) &&
                  view.entry_point() == (KSEG1 | (B & 0x1FFFFFFFu)) &&
                  view.link_segment() == KSEG1 && view.code_data == exe.code_data,
              "the KSEG1 view moves the header and keeps the bytes");
        check(view.read_word(KSEG1 | ((B + 0x18) & 0x1FFFFFFFu)) == exe.read_word(B + 0x18),
              "the view reads the image's words at its own VAs");
    }

    // rebase_codegen_config: exact-match sites naming the image move; the
    // rest is kept as written.
    {
        PSXRecomp::CodeGenConfig cfg;
        cfg.hot_funcs = {B + 0x18, 0x80090000u};
        cfg.mod_function_entry_funcs = {B};
        cfg.ws_cull_bias_sites = {B + 0x28};
        cfg.persist_init_store_sites = {B + 0x28};
        cfg.vsync_query_hle_funcs[B + 0x20] = {1u, 2u, 3u, 4u};
        cfg.ws_bg2d_count_site = B + 0x28;
        cfg.ws_bg2d_init_func = 0;
        PSXRecompV4::WidescreenSignedBoundSite bound{};
        bound.address = B + 0x28;
        cfg.ws_signed_x_bound_sites = {bound};
        const auto v = PSXRecomp::rebase_codegen_config(cfg, exe, KSEG1);
        const uint32_t p = B & 0x1FFFFFFFu;
        check(v.hot_funcs == std::set<uint32_t>{KSEG1 | (p + 0x18), 0x80090000u},
              "an in-image site moves into the segment; an address outside the image stays");
        check(v.mod_function_entry_funcs == std::set<uint32_t>{KSEG1 | p} &&
                  v.ws_cull_bias_sites == std::set<uint32_t>{KSEG1 | (p + 0x28)} &&
                  v.persist_init_store_sites == std::set<uint32_t>{KSEG1 | (p + 0x28)},
              "hook and site sets move");
        check(v.vsync_query_hle_funcs.size() == 1 &&
                  v.vsync_query_hle_funcs.count(KSEG1 | (p + 0x20)) &&
                  v.vsync_query_hle_funcs.at(KSEG1 | (p + 0x20))[3] == 4u,
              "the VSync-query function moves; its data addresses do not");
        check(v.ws_bg2d_count_site == (KSEG1 | (p + 0x28)) && v.ws_bg2d_init_func == 0,
              "scalar sites move; an unset one stays unset");
        check(v.ws_signed_x_bound_sites.size() == 1 &&
                  v.ws_signed_x_bound_sites[0].address == B + 0x28,
              "a physically matched site is kept as written");
        check(!v.split_mid_function_targets && cfg.split_mid_function_targets,
              "a variant compiles the home compile's final pieces: no split pre-pass");
    }

    // overlay_codegen_config: an overlay view in each segment gets every
    // exact-match site that names its bytes, however the config spells it.
    {
        const uint32_t p = B & 0x1FFFFFFFu;
        PSXRecomp::CodeGenConfig cfg;
        cfg.overlay_mode = true;
        cfg.split_mid_function_targets = false;
        cfg.mod_function_entry_funcs = {KUSEG | p, KSEG1 | (p + 0x18)};
        cfg.hot_funcs = {B + 0x20, 0x80090000u, 0xC0000000u | (p + 0x2C)};
        cfg.ws_cull_bias_sites = {0x20000000u | (p + 0x28)};
        cfg.vsync_query_hle_funcs[KSEG1 | (p + 0x20)] = {1u, 2u, 3u, 4u};
        cfg.ws_bg2d_count_site = KUSEG | (p + 0x28);
        PSXRecompV4::WidescreenSignedBoundSite bound{};
        bound.address = KUSEG | (p + 0x28);
        cfg.ws_signed_x_bound_sites = {bound};
        for (uint32_t seg : {KUSEG, 0x80000000u, KSEG1}) {
            const auto view = PSXRecomp::segment_view(exe, seg);
            const auto v = PSXRecomp::overlay_codegen_config(cfg, view);
            const std::string at = " (view " + hex(seg) + ")";
            check(v.mod_function_entry_funcs == std::set<uint32_t>{seg | p, seg | (p + 0x18)},
                  "mod entry hooks spelled in any segment move into the view" + at);
            check(v.hot_funcs == std::set<uint32_t>{seg | (p + 0x20), 0x80090000u,
                                                    0xC0000000u | (p + 0x2C)},
                  "an address outside the image and a KSEG2 spelling stay" + at);
            check(v.ws_cull_bias_sites == cfg.ws_cull_bias_sites,
                  "a spelling in a segment that maps no RAM stays" + at);
            check(v.vsync_query_hle_funcs.size() == 1 &&
                      v.vsync_query_hle_funcs.count(seg | (p + 0x20)),
                  "the VSync-query function moves" + at);
            check(v.ws_bg2d_count_site == (seg | (p + 0x28)), "scalar sites move" + at);
            check(v.ws_signed_x_bound_sites.size() == 1 &&
                      v.ws_signed_x_bound_sites[0].address == (KUSEG | (p + 0x28)),
                  "a physically matched site is kept as written" + at);
            check(v.overlay_mode && !v.split_mid_function_targets,
                  "non-site settings are kept" + at);
        }
        const auto k0 = PSXRecomp::overlay_codegen_config(cfg, exe);
        PSXRecomp::CodeGenConfig same;
        same.mod_function_entry_funcs = {B, B + 0x18};
        same.ws_bg2d_count_site = B + 0x28;
        check(PSXRecomp::overlay_codegen_config(same, exe).mod_function_entry_funcs ==
                      same.mod_function_entry_funcs &&
                  PSXRecomp::overlay_codegen_config(same, exe).ws_bg2d_count_site == B + 0x28 &&
                  k0.mod_function_entry_funcs == same.mod_function_entry_funcs,
              "a KSEG0 view keeps KSEG0-spelled sites as written");
    }

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS: segment variants plan direct-edge closures per segment, rebased\n");
    return 0;
}
