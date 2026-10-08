#include "mod_runtime.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "psx_sha256.h"
#include "psx_icache.h"

#include "gpu.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::array<uint8_t, 2 * 1024 * 1024> ram;
static int failures;
static int activation_calls;
static int plugin_calls;
extern "C" { int g_ls_replay_active; }

extern "C" uint8_t psx_read_byte(uint32_t address) {
    if (address >= 0xc0000000u || (address & 0x1fffffffu) >= 0x800000u) return 0;
    return ram[address & 0x1fffffu];
}

extern "C" void psx_write_byte(uint32_t address, uint8_t value) {
    if (address >= 0xc0000000u || (address & 0x1fffffffu) >= 0x800000u) return;
    ram[address & 0x1fffffu] = value;
}

extern "C" uint16_t psx_read_half(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint16_t)(ram[offset] | ((uint16_t)ram[offset + 1] << 8));
}

extern "C" void psx_write_half(uint32_t address, uint16_t value) {
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
}

extern "C" uint32_t psx_read_word(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint32_t)ram[offset] |
           ((uint32_t)ram[offset + 1] << 8) |
           ((uint32_t)ram[offset + 2] << 16) |
           ((uint32_t)ram[offset + 3] << 24);
}

extern "C" void psx_write_word(uint32_t address, uint32_t value) {
    /* Production memory.c ignores KSEG2 writes outside cache control. */
    if (address >= 0xc0000000u) return;
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
    ram[offset + 2] = (uint8_t)(value >> 16);
    ram[offset + 3] = (uint8_t)(value >> 24);
}

extern "C" uint32_t psx_mod_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t) {
    return 0;
}
extern "C" int psx_ws_x_margin(void) { return 0; }

/* Stand-in for the GPU's display geometry. psx_mod_display_width/height must
 * report exactly what the presenter reports -- a plugin drawing an overlay
 * uses this as the screen edge, so a substituted or rounded value would put
 * HUD elements in the wrong place. */
static GpuDisplayInfo g_test_display;
extern "C" void gpu_get_display_info(GpuDisplayInfo* out) {
    *out = g_test_display;
}

extern "C" void dirty_ram_mark_executable_range(uint32_t, uint32_t) {}
extern "C" int fntrace_is_game_started(void) { return 1; }

static void test_vblank_plugin(void) {
    plugin_calls++;
}

static void test_activation_plugin(void) {
    activation_calls++;
}

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

static void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

static std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-runtime-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::vector<uint8_t> stock(8 * 2352, 0);
    std::vector<uint8_t> overlay(3000);
    for (size_t i = 0; i < overlay.size(); ++i)
        overlay[i] = (uint8_t)(i * 17u + 3u);
    const fs::path stock_path = root / "stock.bin";
    write_bytes(stock_path, stock);
    write_bytes(root / "audio.bin", std::vector<uint8_t>(2 * 2352, 0x5a));
    const fs::path cue_path = root / "stock.cue";
    write_text(cue_path,
        "FILE \"stock.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"audio.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:00\n");
    write_bytes(root / "packages/runtime.test/1.0.0/assets/overlay.bin",
                overlay);
    write_text(root / "packages/runtime.test/1.0.0/manifest.toml",
        "format_version = 5\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Runtime Test\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-RUNTIME\"\n"
        "disc_sha256 = \"" + sha256_hex(stock) + "\"\n"
        "[[feature]]\n"
        "id = \"main-code\"\n"
        "name = \"Main Code\"\n"
        "[[feature]]\n"
        "id = \"disc-byte\"\n"
        "name = \"Disc Byte\"\n"
        "[[feature]]\n"
        "id = \"asset-overlay\"\n"
        "name = \"Asset Overlay\"\n"
        "[[feature]]\n"
        "id = \"user-byte\"\n"
        "name = \"User Byte\"\n"
        "[[feature]]\n"
        "id = \"dynamic-main\"\n"
        "name = \"Dynamic Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-main\"\n"
        "name = \"Sparse Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-flag\"\n"
        "name = \"Sparse Flag\"\n"
        "[[feature]]\n"
        "id = \"sparse-disc\"\n"
        "name = \"Sparse Disc\"\n"
        "[[feature]]\n"
        "id = \"vblank-plugin\"\n"
        "name = \"VBlank Plugin\"\n"
        "[[option]]\n"
        "feature = \"dynamic-main\"\n"
        "id = \"count\"\n"
        "label = \"Count\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 254\n"
        "default = 42\n"
        "[[option]]\n"
        "feature = \"sparse-main\"\n"
        "id = \"frames\"\n"
        "label = \"Frames\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 99\n"
        "default = 2\n"
        "[[patch]]\n"
        "feature = \"main-code\"\n"
        "target = \"main_exe\"\n"
        "address = 2147487744\n"
        "expected = \"01020304\"\n"
        "replace = \"a1a2a3a4\"\n"
        "[[patch]]\n"
        "feature = \"main-code\"\n"
        "target = \"main_exe\"\n"
        "address = 2155872255\n" /* 0x807fffff: last RAM aperture byte */
        "expected = \"0000\"\n"
        "replace = \"abcd\"\n"
        "[[patch]]\n"
        "feature = \"disc-byte\"\n"
        "target = \"disc_raw\"\n"
        "offset = 4714\n"
        "expected = \"aa\"\n"
        "replace = \"bb\"\n"
        "[[patch]]\n"
        "feature = \"user-byte\"\n"
        "target = \"disc_user\"\n"
        "offset = 6154\n"
        "expected = \"cc\"\n"
        "replace = \"dd\"\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488000\n"
        "expected = \"0000\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\" }\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488002\n"
        "expected = \"0100\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\", addend = 1 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, option = \"frames\", encoding = \"u8\" }]\n"
        "when_integer = { option = \"frames\", op = \"gt\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, replace = \"01\" }, "
        "{ offset = 2, replace = \"00\" }]\n"
        "when_integer = { option = \"frames\", op = \"eq\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-flag\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 1, replace = \"42\" }]\n"
        "[[patch]]\n"
        "feature = \"sparse-disc\"\n"
        "target = \"disc_user\"\n"
        "offset = 6164\n"
        "expected = \"11223344\"\n"
        "fields = [{ offset = 0, replace = \"aa\" }, "
        "{ offset = 2, replace = \"bb\" }]\n"
        "[[overlay]]\n"
        "feature = \"asset-overlay\"\n"
        "target = \"disc_raw\"\n"
        "offset = 11408\n"
        "file = \"assets/overlay.bin\"\n"
        "sha256 = \"" + sha256_hex(overlay) + "\"\n"
        "expected_sha256 = \"" +
            sha256_hex(std::vector<uint8_t>(overlay.size(), 0)) + "\"\n"
        "[[plugin]]\n"
        "feature = \"vblank-plugin\"\n"
        "id = \"runtime.test-vblank\"\n");
    write_text(root / "state.toml",
        "format_version = 2\n"
        "[[package]]\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"main-code\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"disc-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"asset-overlay\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"user-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"dynamic-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "count = 42\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "frames = 0\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-flag\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-disc\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"vblank-plugin\"\n"
        "enabled = true\n");

    std::string error;
    PSXRecompV4::mod_clear_plugins_for_tests();
    check(PSXRecompV4::mod_register_activation_plugin(
              "runtime.test-vblank", test_activation_plugin),
          "runtime test activation hook must register");
    check(PSXRecompV4::mod_register_vblank_plugin(
              "runtime.test-vblank", test_vblank_plugin),
          "runtime test plugin must register");
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_disc_digest_computations_for_tests() == 0,
          "initialising the mod step must not fingerprint a disc");
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error),
          "CUE and its data-track BIN must have the same mod target identity");
    check(PSXRecompV4::mod_runtime_disc_digest_computations_for_tests() == 1,
          "a package whose target names a disc_sha256 must still get the "
          "disc digest, once");
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error) &&
              PSXRecompV4::mod_runtime_disc_digest_computations_for_tests() == 1,
          "a second commit of the same disc path must reuse its digest");
    mod_runtime_activate_plugins();
    check(activation_calls == 1,
          "resolved trusted plugin must activate before runtime startup");
    mod_runtime_on_vblank();
    check(plugin_calls == 1,
          "resolved trusted plugin must run on guest VBlank");

    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_dispatch(0x80001000);
    check(ram[0x1000] == 1, "patch must wait for the configured entry point");
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4,
          "main-EXE patch must apply before entry execution");
    check(ram[0x1100] == 42 && ram[0x1101] == 0 &&
              ram[0x1102] == 43 && ram[0x1103] == 0,
          "dynamic main-EXE patches must encode all sites before entry");
    check(ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "adjacent sparse fields must compose while preserving guard-only "
          "bytes");

    /* A full-machine savestate replaces main RAM after the entry-point plan
     * has already applied. Loading a stock checkpoint must not silently turn
     * the currently enabled main-EXE features back off. */
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    psx_icache_bind_memory(ram.data(), (uint32_t)ram.size(), nullptr);
    psx_icache_reset();
    g_psx_icache_active = 1;
    for (uint32_t pc : {0x80001000u, 0x80001100u, 0x80001200u, 0x80001300u}) {
        const unsigned index = (pc >> 2) & 1023u;
        g_psx_icache_tv[index] = pc;
        g_psx_icache_words[index] = psx_read_word(pc);
    }
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "savestate restore must reapply the complete enabled main plan");
    for (uint32_t pc : {0x80001000u, 0x80001100u, 0x80001200u})
        check(psx_icache_read_cached(pc, psx_read_word(pc)) == psx_read_word(pc),
              "restored whole-word and field mod writes must reach instruction fetch");
    check(g_psx_icache_tv[(0x1300u >> 2) & 1023u] == 0x80001300u,
          "mod writes must retain unrelated resident instructions");
    psx_mod_write_code_word(0xa0001300u, 0x24080007u);
    check(psx_icache_read_cached(0x80001300u, psx_read_word(0x80001300u)) == 0x24080007u,
          "plugin code write through an uncached alias must reach cached fetch");
    g_psx_icache_words[(0x1300u >> 2) & 1023u] = 0x2408002au;
    psx_mod_write_code_word(0xc0001300u, 0x24080009u);
    check(psx_read_word(0x80001300u) == 0x24080007u &&
              psx_icache_read_cached(0x80001300u, psx_read_word(0x80001300u)) == 0x2408002au,
          "unmapped plugin code writes must leave RAM and cached instructions untouched");
    /* Run the actual whole-byte plan across the RAM-aperture boundary.
     * Its non-RAM tail must not refresh the mirrored cache word at RAM zero. */
    ram.back() = 0;
    g_psx_icache_tv[0] = 0x80000000u;
    g_psx_icache_words[0] = 0x2409002au;
    g_psx_icache_tv[1023] = 0x807ffffcu;
    g_psx_icache_words[1023] = psx_read_word(0x807ffffcu);
    mod_runtime_on_savestate_loaded();
    check(ram.back() == 0xabu &&
              psx_icache_read_cached(0x807ffffcu, psx_read_word(0x807ffffcu)) == psx_read_word(0x807ffffcu),
          "main-plan boundary write must refresh its changed RAM byte");
    check(psx_icache_read_cached(0x80000000u, psx_read_word(0x80000000u)) == 0x2409002au,
          "main-plan RAM-aperture tail must retain unrelated cached RAM zero");

    std::array<uint8_t, 2352> sector{};
    sector[10] = 0xaa;
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xaa, "disc overlay must stay off during reference reads");
    mod_runtime_enable_disc_patches();
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xbb, "raw disc overlay must patch matching sectors");

    std::array<uint8_t, 2352> overlay_sector{};
    mod_runtime_patch_disc_sector(
        4, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[1999] == 0 &&
              overlay_sector[2000] == overlay[0] &&
              overlay_sector[2351] == overlay[351],
          "file overlay must patch the tail of its first sector");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        5, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector.front() == overlay[352] &&
              overlay_sector.back() == overlay[2703],
          "file overlay must patch complete middle sectors");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        6, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[0] == overlay[2704] &&
              overlay_sector[295] == overlay[2999] &&
              overlay_sector[296] == 0,
          "file overlay must patch the head of its final sector");

    std::array<uint8_t, 2352> mode2_sector{};
    mode2_sector[15] = 2;
    mode2_sector[18] = 0;
    mode2_sector[24 + 10] = 0xcc;
    mode2_sector[24 + 20] = 0x11;
    mode2_sector[24 + 21] = 0x22;
    mode2_sector[24 + 22] = 0x33;
    mode2_sector[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, mode2_sector.data(), (uint32_t)mode2_sector.size());
    check(mode2_sector[24 + 10] == 0xdd,
          "disc_user operations must apply to raw Mode2 Form1 user data");
    check(mode2_sector[24 + 20] == 0xaa &&
              mode2_sector[24 + 21] == 0x22 &&
              mode2_sector[24 + 22] == 0xbb &&
              mode2_sector[24 + 23] == 0x44,
          "sparse disc writes must validate a complete guard and modify only "
          "owned fields");
    std::array<uint8_t, 2352> audio_sector{};
    audio_sector[24 + 10] = 0xcc;
    mod_runtime_patch_disc_sector(
        3, 1, audio_sector.data(), (uint32_t)audio_sector.size());
    check(audio_sector[24 + 10] == 0xcc,
          "disc_user operations must not modify CDDA/non-data sectors");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 2; ram[0x1103] = 0; /* second dynamic guard is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1100] == 0 && ram[0x1101] == 0,
          "one failed generated guard must leave the complete main plan untouched");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x33; /* guard-only byte is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1200] == 2 && ram[0x1201] == 0 &&
              ram[0x1202] == 1 && ram[0x1203] == 0x33,
          "a failed sparse guard-only byte must leave the complete main plan "
          "untouched");

    /* Loading can be requested while the boot executable is still running,
     * before the configured game entry point has dispatched. The restored
     * checkpoint itself supplies the bytes used to validate and apply the
     * plan in that case. */
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "pre-entry savestate restore must validate and apply the main plan");

    mod_runtime_enable_disc_patches();
    std::array<uint8_t, 2352> bad_sparse_disc{};
    bad_sparse_disc[15] = 2;
    bad_sparse_disc[18] = 0;
    bad_sparse_disc[24 + 10] = 0xcc;
    bad_sparse_disc[24 + 20] = 0x11;
    bad_sparse_disc[24 + 21] = 0x99; /* guard-only byte is wrong */
    bad_sparse_disc[24 + 22] = 0x33;
    bad_sparse_disc[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, bad_sparse_disc.data(),
        (uint32_t)bad_sparse_disc.size());
    check(bad_sparse_disc[24 + 10] == 0xcc &&
              bad_sparse_disc[24 + 20] == 0x11 &&
              bad_sparse_disc[24 + 22] == 0x33,
          "a failed sparse disc guard must leave every write in the sector "
          "untouched");

    /*
     * Display geometry passthrough. Ape Escape scans out 384 while its mode
     * width is 368, which is precisely why a plugin cannot derive this from
     * GPUSTAT: the horizontal range that makes the difference is write-only.
     */
    g_test_display.width = 384;
    g_test_display.height = 240;
    check(psx_mod_display_width() == 384u,
          "psx_mod_display_width must report the presenter's visible width, "
          "not the coarse mode width");
    check(psx_mod_display_height() == 240u,
          "psx_mod_display_height must report the presenter's visible height");

    g_test_display.width = 0;
    g_test_display.height = 0;
    check(psx_mod_display_width() == 0u && psx_mod_display_height() == 0u,
          "unestablished display geometry must report zero so callers skip "
          "drawing instead of guessing");

    /* PS1B-340: the disc digest costs a decode of the whole image, and it used
     * to be computed at every start whether a package read it or not. It is
     * now computed only when an enabled package needs it. The package above
     * names a disc_sha256, so each start with it still fingerprints the disc
     * and a wrong disc is still refused. */
    {
        using PSXRecompV4::mod_runtime_disc_digest_computations_for_tests;
        std::vector<uint8_t> other_disc(8 * 2352, 0);
        other_disc[100] = 1;
        const fs::path other_path = root / "other.bin";
        write_bytes(other_path, other_disc);
        check(PSXRecompV4::mod_runtime_initialize(
                  root, "SLUS-RUNTIME", 0x80002000, {}, &error),
              error.c_str());
        const unsigned before_wrong =
            mod_runtime_disc_digest_computations_for_tests();
        std::string refusal;
        check(!PSXRecompV4::mod_runtime_commit(other_path, &refusal) &&
                  refusal.find("package does not target this game/image: "
                               "runtime.test") != std::string::npos,
              "a disc with another digest must still be refused by a package "
              "that names its disc");
        check(mod_runtime_disc_digest_computations_for_tests() ==
                  before_wrong + 1,
              "refusing a wrong disc takes exactly one disc fingerprint");
        check(PSXRecompV4::mod_runtime_commit(stock_path, &error),
              "the right disc must be accepted after a refused one");
        check(mod_runtime_disc_digest_computations_for_tests() ==
                  before_wrong + 2,
              "a changed disc path must be fingerprinted again");

        /* A package that names no disc: any disc is its target, so nothing
         * reads the digest and the image is not decoded. */
        const fs::path open_root = root / "open-target";
        write_text(open_root / "packages/open.test/1.0.0/manifest.toml",
            "format_version = 5\n"
            "id = \"open.test\"\n"
            "version = \"1.0.0\"\n"
            "name = \"Open Target\"\n"
            "[[target]]\n"
            "game_id = \"SLUS-RUNTIME\"\n"
            "[[feature]]\n"
            "id = \"main-code\"\n"
            "name = \"Main Code\"\n"
            "[[patch]]\n"
            "feature = \"main-code\"\n"
            "target = \"main_exe\"\n"
            "address = 2147488768\n"
            "expected = \"0a0b\"\n"
            "replace = \"c1c2\"\n");
        write_text(open_root / "state.toml",
            "format_version = 2\n"
            "[[package]]\n"
            "id = \"open.test\"\n"
            "version = \"1.0.0\"\n"
            "[[feature]]\n"
            "package_id = \"open.test\"\n"
            "id = \"main-code\"\n"
            "enabled = true\n");
        check(PSXRecompV4::mod_runtime_initialize(
                  open_root, "SLUS-RUNTIME", 0x80002000, {}, &error),
              error.c_str());
        const unsigned before_open =
            mod_runtime_disc_digest_computations_for_tests();
        check(PSXRecompV4::mod_runtime_commit(stock_path, &error),
              error.c_str());
        check(PSXRecompV4::mod_runtime_commit(other_path, &error),
              "a package that names no disc must accept any disc");
        check(mod_runtime_disc_digest_computations_for_tests() == before_open,
              "a start whose enabled packages name no disc must not "
              "fingerprint the disc");
        ram[0x1400] = 0x0a; ram[0x1401] = 0x0b;
        mod_runtime_on_dispatch(0x80002000);
        check(ram[0x1400] == 0xc1 && ram[0x1401] == 0xc2,
              "a plan resolved without the disc digest must still apply");

        /* No package at all: the state of every product as shipped that has
         * no enabled mod. */
        const fs::path empty_root = root / "no-packages";
        fs::create_directories(empty_root);
        check(PSXRecompV4::mod_runtime_initialize(
                  empty_root, "SLUS-RUNTIME", 0x80002000, {}, &error),
              error.c_str());
        check(PSXRecompV4::mod_runtime_commit(stock_path, &error),
              error.c_str());
        check(mod_runtime_disc_digest_computations_for_tests() == before_open,
              "a start with no mod package must not fingerprint the disc");
    }

    /* Source-owned ISO: nested file spanning two sectors, without a derived
     * disc. Host reads must choose the original mount and leave sizes honest. */
    std::vector<uint8_t> iso(24*2048);
    auto le32 = [&](size_t at,uint32_t n) { for(unsigned i=0;i<4;++i) iso[at+i]=(uint8_t)(n>>(i*8)); };
    auto record = [&](size_t at,uint32_t lba,uint32_t bytes,bool directory,const std::string& name) {
        iso[at]=(uint8_t)((33+name.size()+1)&~size_t(1));
        le32(at+2,lba); le32(at+10,bytes); iso[at+25]=directory?2:0;
        iso[at+28]=1;iso[at+31]=1;iso[at+32]=(uint8_t)name.size();
        std::copy(name.begin(),name.end(),iso.begin()+at+33);
    };
    iso[16*2048]=1;std::copy_n("CD001",5,iso.begin()+16*2048+1);iso[16*2048+6]=1;
    record(16*2048+156,20,2048,true,std::string(1,'\0'));
    record(20*2048,21,2048,true,"S0");
    record(21*2048,22,3000,false,"LEVEL.NSF;1");
    for(unsigned i=0;i<3000;++i)iso[22*2048+i]=(uint8_t)(i*7);
    const auto disc_root=root/"host-reader";
    const auto iso_path=disc_root/"original.iso";
    write_bytes(iso_path,iso);
    check(PSXRecompV4::mod_runtime_initialize(disc_root,"READER",0,{},&error),"reader initialize");
    check(PSXRecompV4::mod_runtime_commit(iso_path,&error),"reader mount original ISO");
    uint32_t bytes=0;
    check(psx_mod_read_disc_file("S0/LEVEL.NSF",nullptr,0,&bytes) && bytes==3000,"query original nested file size");
    std::vector<uint8_t> result(3000);
    check(!psx_mod_read_disc_file("S0/LEVEL.NSF",result.data(),2999,&bytes) && bytes==0,"undersized destination rejected");
    check(psx_mod_read_disc_file("S0/LEVEL.NSF",result.data(),(uint32_t)result.size(),&bytes) &&
          bytes==3000 && std::equal(result.begin(),result.end(),iso.begin()+22*2048),"complete original file bytes");
    check(!psx_mod_read_disc_file("S0/MISSING.NSF",nullptr,0,&bytes) && bytes==0,"missing file explicit");
    check(!psx_mod_read_disc_file("S0",nullptr,0,&bytes),"directory rejected");
    std::vector<uint8_t> raw_iso(24*2352);
    for(unsigned i=0;i<24;++i) {
        raw_iso[i*2352+15]=2;
        std::copy_n(iso.begin()+i*2048,2048,raw_iso.begin()+i*2352+24);
    }
    const auto raw_path=disc_root/"original.bin";
    write_bytes(raw_path,raw_iso);
    check(PSXRecompV4::mod_runtime_commit(raw_path,&error),"reader mount raw disc");
    check(psx_mod_read_disc_file("S0/LEVEL.NSF",result.data(),(uint32_t)result.size(),&bytes) &&
          std::equal(result.begin(),result.end(),iso.begin()+22*2048),"raw and ISO reads identical");
    fs::remove_all(root, ec);
    if (failures) return 1;
    std::cout << "mod runtime tests passed\n";
    return 0;
}
