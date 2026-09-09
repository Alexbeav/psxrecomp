#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "../src/bios_address_model.h"
#include "../src/config_loader.h"
#include "../src/full_function_emitter.h"
#include "../src/function_discovery.h"

using PSXRecompV4::BiosAddressModel;
using PSXRecompV4::BiosAddrCopy;
using PSXRecompV4::BiosConfig;
using PSXRecompV4::DiscoveredFunction;
using PSXRecompV4::DiscoveryResult;
using PSXRecompV4::EmitStats;
using PSXRecompV4::FullFunctionEmitter;
using PSXRecompV4::FunctionDiscovery;

namespace {

constexpr uint32_t kBase = 0xBFC00000u;
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void append_word(std::vector<uint8_t>& rom, uint32_t word) {
    rom.push_back(static_cast<uint8_t>(word));
    rom.push_back(static_cast<uint8_t>(word >> 8));
    rom.push_back(static_cast<uint8_t>(word >> 16));
    rom.push_back(static_cast<uint8_t>(word >> 24));
}

DiscoveredFunction function_at(uint32_t entry, uint32_t end,
                               std::initializer_list<uint32_t> leaders) {
    DiscoveredFunction fn{};
    fn.entry_addr = entry;
    fn.normalized_addr = entry & 0x1FFFFFFFu;
    fn.end_addr = end;
    fn.instruction_count = (end - entry) / 4u + 1u;
    fn.termination_reason = "jr_ra";
    fn.discovered_by = "synthetic test";
    fn.block_leaders.assign(leaders.begin(), leaders.end());
    return fn;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

struct RunResult {
    EmitStats stats;
    std::string dispatch;
    std::string body;
};

RunResult run_case(const char* name, const std::vector<uint32_t>& words,
                   std::vector<DiscoveredFunction> functions) {
    std::vector<uint8_t> rom;
    for (uint32_t word : words) append_word(rom, word);

    DiscoveryResult discovery{};
    discovery.ok = true;
    discovery.functions = std::move(functions);

    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const auto out_dir = std::filesystem::temp_directory_path() /
                         ("psxrecomp-full-emitter-" + std::string(name) + "-" +
                          std::to_string(nonce));
    std::filesystem::create_directories(out_dir);

    const std::string stem = "Test";
    RunResult result;
    result.stats = FullFunctionEmitter::emit(
        rom, kBase, kBase + static_cast<uint32_t>(rom.size()) - 1u,
        discovery, "synthetic", out_dir.string(), stem);
    result.dispatch = read_file(out_dir / (stem + "_dispatch.c"));
    result.body = read_file(out_dir / (stem + "_full.c"));
    std::filesystem::remove_all(out_dir);
    return result;
}

void expect_entry_absent(const RunResult& result, uint32_t normalized,
                         const char* message) {
    char needle[96];
    std::snprintf(needle, sizeof(needle),
                  "{ 0x%08Xu, Test_func_%08X", normalized, normalized);
    expect(result.dispatch.find(needle) == std::string::npos, message);
}

void expect_dispatch_key_absent(const RunResult& result, uint32_t normalized,
                                const char* message) {
    char needle[32];
    std::snprintf(needle, sizeof(needle), "{ 0x%08Xu,", normalized);
    expect(result.dispatch.find(needle) == std::string::npos, message);
}

void delay_slot_load_falls_back() {
    const auto result = run_case(
        "delay-slot",
        {
            0x10000001u,  // beq zero,zero,+1
            0x8D280000u,  // lw t0,0(t1) -- branch delay slot
            0x01001021u,  // addu v0,t0,zero -- dependent successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 16u, {kBase, kBase + 8u})});
    expect(result.stats.functions_interpreted == 1,
           "delay-slot load marks the function for interpretation");
    expect(result.stats.functions_emitted == 0,
           "delay-slot load function is not emitted");
    expect_entry_absent(result, 0x00000500u,
                        "delay-slot load function is absent from dispatch");
    expect_dispatch_key_absent(result, 0x00000508u,
                               "delay-slot successor continuation is absent from dispatch");
}

void label_split_load_falls_back() {
    const auto result = run_case(
        "label-split",
        {
            0x8D280000u,  // lw t0,0(t1)
            0x01001021u,  // addu v0,t0,zero -- dependent labeled successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase, kBase + 4u})});
    expect(result.stats.functions_interpreted == 1,
           "label-split load marks the function for interpretation");
    expect_entry_absent(result, 0x00000500u,
                        "label-split function is absent from dispatch");
    expect_dispatch_key_absent(result, 0x00000504u,
                               "label-split continuation is absent from dispatch");
}

void fragment_split_load_falls_back() {
    const auto result = run_case(
        "fragment-split",
        {
            0x8D280000u,  // fragment 1: lw t0,0(t1)
            0x01001021u,  // fragment 2: dependent successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {
            function_at(kBase, kBase, {kBase}),
            function_at(kBase + 4u, kBase + 12u, {kBase + 4u}),
        });
    expect(result.stats.functions_interpreted == 1,
           "fragment-split load marks only its owning function for interpretation");
    expect(result.stats.functions_emitted == 1,
           "unaffected neighboring fragment remains native");
    expect_entry_absent(result, 0x00000500u,
                        "fragment-split load owner is absent from dispatch");
}

void noncomplementary_lwl_falls_back() {
    const auto result = run_case(
        "lwl-dependent",
        {
            0x89280000u,  // lwl t0,0(t1)
            0x01001021u,  // addu v0,t0,zero -- dependent, not lwr
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase})});
    expect(result.stats.functions_interpreted == 1,
           "non-complementary LWL dependency falls back");
    expect_entry_absent(result, 0x00000500u,
                        "non-complementary LWL function is absent from dispatch");
}

void complementary_lwl_lwr_stays_native() {
    const auto result = run_case(
        "lwl-lwr",
        {
            0x89280000u,  // lwl t0,0(t1)
            0x99280003u,  // lwr t0,3(t1) -- architectural forwarding pair
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase})});
    expect(result.stats.functions_interpreted == 0,
           "complementary LWL/LWR does not fall back");
    expect(result.stats.functions_emitted == 1,
           "complementary LWL/LWR remains native");
}

void linear_fetches_follow_runtime_address(uint32_t runtime) {
    const bool uncached = runtime >= 0xA0000000u;
    const auto result = run_case(
        uncached ? "uncached-fetch" : "relocated-cached-fetch",
        {0x24080001u, 0x25080001u, 0x25080001u, 0x25080001u,
         0x03E00008u, 0x00000000u},
        {function_at(kBase, kBase + 20u, {kBase})});
    expect(result.stats.functions_emitted == 1, "synthetic fetch program remains native");
    if (const char* output = std::getenv("PSX_TEST_FETCH_OUTPUT_DIR")) {
        char name[64]; std::snprintf(name,sizeof name,"full-fetch-%08X.c",runtime);
        std::ofstream generated(std::filesystem::path(output)/name);generated << result.body;
    }
    for (uint32_t offset = 0; offset < 24; offset += 4) {
        char needle[96];
        std::snprintf(needle, sizeof needle, "psx_icache_fetch(cpu, 0x%08Xu);", runtime + offset);
        const auto first = result.body.find(needle);
        const bool required = uncached || (offset & 15u) == 0;
        expect((first != std::string::npos) == required,
               uncached ? "every uncached instruction pays a fetch, including return slot"
                        : "relocated cached followers retain the existing leader policy");
        if (first != std::string::npos)
            expect(result.body.find(needle, first + 1) == std::string::npos,
                   "a fetch is not emitted twice for one instruction");
    }
}

}  // namespace

int main(int argc, char** argv) {
    BiosConfig config{};
    config.config_path = "test://full-function-emitter";
    config.load_address = kBase;
    config.text_size = 0x1000u;
    BiosAddrCopy copy;
    copy.name = "synthetic RAM-backed BIOS code";
    copy.rom_lo = 0x1FC00000u;
    copy.rom_hi = 0x1FC01000u;
    copy.ram_lo = 0x00000500u;
    copy.runtime_base = 0x00000500u;
    copy.key_is_ram = true;
    config.address_copies.push_back(copy);
    BiosAddressModel model = BiosAddressModel::from_config(config);
    FunctionDiscovery::set_address_model(&model);
    FullFunctionEmitter::set_address_model(&model);

    if (argc == 3 && std::string(argv[2]).rfind("rom-irq", 0) == 0) {
        config.address_copies.clear();
        BiosAddressModel rom_model = BiosAddressModel::from_config(config);
        FunctionDiscovery::set_address_model(&rom_model);
        FullFunctionEmitter::set_address_model(&rom_model);
        std::vector<uint32_t> words(0x564u / 4u, 0u);
        const uint32_t branch = std::string(argv[2]) == "rom-irq-not-taken" ?
            0x13000001u : std::string(argv[2]) == "rom-irq-jump" ? 0x0BF00155u : 0x17000001u;
        const uint32_t body[] = {0x27180001u, 0x27180001u, 0x27180001u,
            branch, 0u, 0x24190009u, 0xAE190000u, 0x03E00008u, 0u};
        for (unsigned i = 0; i < 9; ++i) words[0x540u / 4u + i] = body[i];
        std::vector<uint8_t> rom;
        for (uint32_t op : words) append_word(rom, op);
        DiscoveryResult discovery{}; discovery.ok = true;
        discovery.functions = {
            function_at(kBase + 0x540u, kBase + 0x550u, {kBase + 0x540u}),
            function_at(kBase + 0x554u, kBase + 0x560u, {kBase + 0x554u})};
        std::filesystem::create_directories(argv[1]);
        FullFunctionEmitter::emit(rom, kBase, kBase + (uint32_t)rom.size() - 1u,
            discovery, "authored ROM IRQ fixture", argv[1], "Test");
        return 0;
    }

    // A branch-slot return-address load crossing to a represented epilogue.
    // The first target instruction also reads RA: retiring at handoff is early.
    if (argc == 3 && (std::string(argv[2]) == "slice-load-return" ||
                     std::string(argv[2]) == "slice-load-return-dependent")) {
        config.address_copies[0].runtime_base = 0x80000500u;
        BiosAddressModel return_model = BiosAddressModel::from_config(config);
        FunctionDiscovery::set_address_model(&return_model);
        FullFunctionEmitter::set_address_model(&return_model);
        std::vector<uint32_t> words = {
            0x27BDFFE8u, 0x10000003u, 0x8FBF0014u, 0u, 0u,
            std::string(argv[2]) == "slice-load-return-dependent" ? 0x03E08021u : 0x00008021u,
            0x27BD0018u, 0x03E00008u, 0x26310001u,
            0x26520001u, 0x0260F821u, 0x03E00008u, 0u};
        std::vector<uint8_t> rom;
        for (uint32_t op : words) append_word(rom, op);
        DiscoveryResult discovery{}; discovery.ok = true;
        discovery.functions = {
            function_at(kBase, kBase + 8u, {kBase}),
            function_at(kBase + 0x14u, kBase + 0x20u, {kBase + 0x14u}),
            function_at(kBase + 0x24u, kBase + 0x30u, {kBase + 0x24u})};
        std::filesystem::create_directories(argv[1]);
        FullFunctionEmitter::emit(rom, kBase, kBase + (uint32_t)rom.size() - 1u,
                                 discovery, "authored slice return fixture", argv[1], "Test");
        std::ofstream data(std::filesystem::path(argv[1]) / "authored-ram.bin", std::ios::binary);
        data.write((const char*)rom.data(), rom.size());
        return 0;
    }

    // Authored copied-BIOS timer IRQ recognition, with a single native group.
    // Target/mode/mask are case inputs, never fitted to a retail outcome.
    if (argc == 3 && std::string(argv[2]) == "irq-recognition") {
        config.address_copies[0].runtime_base = 0x80000500u;
        BiosAddressModel irq_model = BiosAddressModel::from_config(config);
        FunctionDiscovery::set_address_model(&irq_model);
        FullFunctionEmitter::set_address_model(&irq_model);
        std::vector<uint32_t> words = {
            0xA5690008u, 0xA56C0004u, 0x95680000u, 0x8E020000u, 0x8E180000u,
            0x00000000u, 0x2719FFFFu, 0x14400005u, 0xAE190000u,
            0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
            0x03E00008u, 0x26310001u};
        std::vector<uint8_t> rom;
        for (uint32_t op : words) append_word(rom, op);
        DiscoveryResult discovery{}; discovery.ok = true;
        discovery.functions = {
            function_at(kBase, kBase + 0x20u, {kBase}),
            function_at(kBase + 0x34u, kBase + 0x38u, {kBase + 0x34u})};
        std::filesystem::create_directories(argv[1]);
        FullFunctionEmitter::emit(rom, kBase, kBase + (uint32_t)rom.size() - 1u,
                                 discovery, "authored timer IRQ fixture", argv[1], "Test");
        std::ofstream data(std::filesystem::path(argv[1]) / "authored-ram.bin", std::ios::binary);
        data.write((const char*)rom.data(), rom.size());
        return 0;
    }

    // Authored uncached SYS resume and ordinary JAL continuation controls.
    if (argc == 3 && std::string(argv[2]) == "syscall-resume") {
        config.address_copies.clear();
        BiosAddressModel resume_model = BiosAddressModel::from_config(config);
        FunctionDiscovery::set_address_model(&resume_model);
        FullFunctionEmitter::set_address_model(&resume_model);
        std::vector<uint32_t> words(0x48u / 4u, 0u);
        words[0] = 0x0000000Cu; words[1] = 0x03E00008u; words[2] = 0x26310001u;
        words[8] = 0x0FF00010u; words[10] = 0x02000008u; words[11] = 0x26310001u;
        words[16] = 0x03E00008u;
        std::vector<uint8_t> rom;
        for (uint32_t op : words) append_word(rom, op);
        DiscoveryResult discovery{}; discovery.ok = true;
        discovery.functions = {
            function_at(kBase, kBase + 8u, {kBase}),
            function_at(kBase + 0x20u, kBase + 0x2Cu, {kBase + 0x20u, kBase + 0x28u}),
            function_at(kBase + 0x40u, kBase + 0x44u, {kBase + 0x40u})};
        std::filesystem::create_directories(argv[1]);
        FullFunctionEmitter::emit(rom, kBase, kBase + (uint32_t)rom.size() - 1u,
                                 discovery, "authored syscall resume fixture", argv[1], "Test");
        std::ofstream data(std::filesystem::path(argv[1]) / "authored-rom.bin", std::ios::binary);
        data.write((const char*)rom.data(), rom.size());
        return 0;
    }

    // Export only authored instructions for a real-runtime admission fixture.
    if (argc == 2) {
        std::vector<uint32_t> words(0x130u / 4u, 0u);
        const auto put = [&](uint32_t at, std::initializer_list<uint32_t> ops) {
            for (uint32_t op : ops) { words[(at - 0x500u) / 4u] = op; at += 4u; }
        };
        put(0x500u, {0x3C080000u, 0x25080598u, 0x01000008u, 0u});
        put(0x540u, {0x0C000160u, 0u, 0x03E08021u, 0x02200008u, 0u});
        put(0x580u, {0x03E00008u, 0u});
        put(0x5C0u, {0x0000000Cu, 0x03E00008u, 0u});
        put(0x600u, {0x10000007u, 0u});
        put(0x620u, {0x01001021u, 0x03E00008u, 0u});
        std::vector<uint8_t> rom;
        for (uint32_t op : words) append_word(rom, op);
        DiscoveryResult discovery{}; discovery.ok = true;
        discovery.functions = {
            function_at(kBase, kBase + 12u, {kBase}),
            function_at(kBase + 0x40u, kBase + 0x50u, {kBase + 0x40u, kBase + 0x48u}),
            function_at(kBase + 0x80u, kBase + 0x84u, {kBase + 0x80u}),
            function_at(kBase + 0xC0u, kBase + 0xC8u, {kBase + 0xC0u}),
            function_at(kBase + 0x100u, kBase + 0x128u, {kBase + 0x100u, kBase + 0x120u})};
        std::filesystem::create_directories(argv[1]);
        FullFunctionEmitter::emit(rom, kBase, kBase + (uint32_t)rom.size() - 1u,
                                 discovery, "authored alias fixture", argv[1], "Test");
        std::ofstream data(std::filesystem::path(argv[1]) / "authored-ram.bin", std::ios::binary);
        data.write((const char*)rom.data(), rom.size());
        return 0;
    }

    delay_slot_load_falls_back();
    label_split_load_falls_back();
    fragment_split_load_falls_back();
    noncomplementary_lwl_falls_back();
    complementary_lwl_lwr_stays_native();
    linear_fetches_follow_runtime_address(0x500u);
    for (const uint32_t alias : {0x80010000u, 0xA0010000u}) {
        config.address_copies[0].ram_lo = 0x00010000u;
        config.address_copies[0].runtime_base = alias;
        BiosAddressModel alias_model = BiosAddressModel::from_config(config);
        FunctionDiscovery::set_address_model(&alias_model);
        FullFunctionEmitter::set_address_model(&alias_model);
        linear_fetches_follow_runtime_address(alias);
    }
    config.address_copies.clear();
    BiosAddressModel uncached_model = BiosAddressModel::from_config(config);
    FunctionDiscovery::set_address_model(&uncached_model);
    FullFunctionEmitter::set_address_model(&uncached_model);
    linear_fetches_follow_runtime_address(kBase);

    const auto resume = run_case("syscall-resume", {0x0000000Cu, 0x03E00008u, 0x26310001u},
                                {function_at(kBase, kBase + 8u, {kBase})});
    expect(resume.body.find("case 0xBFC00004u: goto label_BFC00004;") != std::string::npos,
           "post-syscall resume enters its owning function at the exact instruction");
    expect(resume.dispatch.find("{ 0x1FC00004u,") != std::string::npos,
           "post-syscall interior instruction has a dispatch key");

    if (failures != 0) {
        std::fprintf(stderr, "%d full-function emitter test(s) failed\n", failures);
        return 1;
    }
    std::puts("Full-function emitter fail-closed tests passed");
    return 0;
}
