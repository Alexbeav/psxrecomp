#include "mod_packages.h"
#include "psx_sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static int failures;

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

static void write_deflated_package(const fs::path& path) {
    static const char* compressed_hex =
        "4bcb2fca4d2c892f4b2d2acecccf53b05530e4ca4c01524a5599057ab9f929"
        "4a5c082925433d033d0325aebcc4dc541037ca3340c117a4a428b5383f07a80"
        "e2498929a9c93589458925996aac4151d5d9258949e5a121bcb950ed4140f313"
        "ad827345837c4353844890b00";
    std::vector<uint8_t> compressed;
    for (const char* p = compressed_hex; *p; p += 2)
        compressed.push_back((uint8_t)std::stoul(std::string(p, 2), nullptr, 16));
    std::vector<uint8_t> zip;
    auto le16 = [&](uint16_t v) {
        zip.push_back((uint8_t)v); zip.push_back((uint8_t)(v >> 8));
    };
    auto le32 = [&](uint32_t v) {
        le16((uint16_t)v); le16((uint16_t)(v >> 16));
    };
    const std::string name = "manifest.toml";
    le32(0x04034b50); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), compressed.begin(), compressed.end());
    const uint32_t central_offset = (uint32_t)zip.size();
    le32(0x02014b50); le16(20); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0); le16(0); le16(0); le16(0);
    le32(0); le32(0);
    zip.insert(zip.end(), name.begin(), name.end());
    const uint32_t central_size = (uint32_t)zip.size() - central_offset;
    le32(0x06054b50); le16(0); le16(0); le16(1); le16(1);
    le32(central_size); le32(central_offset); le16(0);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)zip.data(), (std::streamsize)zip.size());
}

static std::string manifest(const std::string& id, const std::string& version,
                            const std::string& extra = {}) {
    return
        "format_version = 1\n"
        "id = \"" + id + "\"\n"
        "version = \"" + version + "\"\n"
        "name = \"" + id + "\"\n"
        "resolver = \"declarative\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-TEST\"\n" + extra;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-package-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    {
        const uint8_t abc[] = {'a', 'b', 'c'};
        uint8_t one_shot[32], streamed[32];
        psx_sha256_compute(abc, sizeof(abc), one_shot);
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        psx_sha256_update(&hash, abc, 1);
        psx_sha256_update(&hash, abc + 1, 2);
        psx_sha256_final(&hash, streamed);
        check(std::equal(one_shot, one_shot + 32, streamed),
              "streaming SHA-256 must match one-shot hashing");
    }

    write_text(root / "packages/base.mod/1.0.0/manifest.toml",
               manifest("base.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"difficulty\"\n"
                   "label = \"Difficulty\"\n"
                   "type = \"choice\"\n"
                   "default = \"normal\"\n"
                   "[[option.choice]]\nvalue = \"normal\"\nlabel = \"Normal\"\n"
                   "[[option.choice]]\nvalue = \"hard\"\nlabel = \"Hard\"\n"
                   "[[patch]]\n"
                   "target = \"main_exe\"\n"
                   "address = 2147487744\n"
                   "expected = \"01 02 03 04\"\n"
                   "replace = \"05 06 07 08\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"
                   "[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/base.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 123456\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"));
    write_text(root / "packages/base.mod/1.0.0/assets/base.xdelta3", "test");
    write_text(root / "packages/addon.mod/2.0.0/manifest.toml",
               manifest("addon.mod", "2.0.0",
                   "\n[[dependency]]\nid = \"base.mod\"\nversion = \"^1.0.0\"\n"));

    ModPackageManager manager(root);
    std::string error;
    check(manager.scan(&error), error.c_str());
    write_deflated_package(root / "zip.psxmod");
    check(manager.install_archive(root / "zip.psxmod", nullptr, nullptr, &error),
          error.c_str());
    check(manager.packages().count("zip.mod") == 1,
          "deflated .psxmod must install");
    if (const char* external = std::getenv("PSXMOD_TEST_ARCHIVE");
        external && external[0]) {
        std::string installed_id, installed_version;
        check(manager.install_archive(external, &installed_id, &installed_version,
                                      &error),
              error.c_str());
        check(!installed_id.empty() && !installed_version.empty(),
              "external package must report installed identity");
    }
    check(manager.load_state(&error), error.c_str());
    check(manager.set_enabled("addon.mod", true, &error), error.c_str());
    ModResolution missing = manager.resolve("SLUS-TEST");
    check(!missing.ok, "missing dependency must fail resolution");
    check(manager.set_enabled("base.mod", true, &error), error.c_str());
    check(manager.set_option("base.mod", "difficulty", "hard", &error), error.c_str());
    check(!manager.set_option("base.mod", "difficulty", "impossible", &error),
          "invalid choice must be rejected");

    ModResolution resolved = manager.resolve("SLUS-TEST");
    check(resolved.ok, "valid dependency graph must resolve");
    check(resolved.ordered.size() == 2, "two packages should resolve");
    check(resolved.ordered.size() == 2 && resolved.ordered[0]->id == "base.mod",
          "dependency must precede dependent");
    check(resolved.writes.size() == 1, "selected declarative patch must resolve");
    check(resolved.writes.size() == 1 &&
              resolved.writes[0].location == 0x80001000ull &&
              resolved.writes[0].replacement[0] == 5,
          "resolved write must retain guest address and bytes");
    check(resolved.derived_discs.size() == 1 &&
              resolved.derived_discs[0].output_size == 123456,
          "selected derived-disc recipe must resolve");
    check(resolved.fingerprint.size() == 64, "plan fingerprint must be SHA-256 hex");
    const std::string fingerprint = resolved.fingerprint;

    check(manager.save_state(&error), error.c_str());
    ModPackageManager reload(root);
    check(reload.scan(&error), error.c_str());
    check(reload.load_state(&error), error.c_str());
    check(reload.resolve("SLUS-TEST").fingerprint == fingerprint,
          "saved state must resolve deterministically");
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "active package cannot be removed");
    check(reload.set_enabled("base.mod", false, &error), error.c_str());
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "enabled dependent must protect required version");
    check(reload.set_enabled("addon.mod", false, &error), error.c_str());
    check(reload.remove_version("base.mod", "1.0.0", &error), error.c_str());

    write_text(root / "packages/conflict.a/1.0.0/manifest.toml",
               "format_version = 1\n"
               "id = \"conflict.a\"\n"
               "version = \"1.0.0\"\n"
               "name = \"conflict.a\"\n"
               "resolver = \"declarative\"\n"
               "conflicts = [\"conflict.b\"]\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n");
    write_text(root / "packages/conflict.b/1.0.0/manifest.toml",
               manifest("conflict.b", "1.0.0"));
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", true, &error), error.c_str());
    check(reload.set_enabled("conflict.b", true, &error), error.c_str());
    check(reload.selections().at("conflict.a").enabled &&
              reload.selections().at("conflict.b").enabled,
          "enabling a package must not silently disable another package");
    check(!reload.resolve("SLUS-TEST").ok,
          "declared conflicts must fail resolution");

    write_text(root / "packages/matrix.mod/1.0.0/manifest.toml",
               manifest("matrix.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"title\"\n"
                   "label = \"Title\"\n"
                   "type = \"choice\"\n"
                   "default = \"mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"mega\"\n"
                   "label = \"Mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"rockman\"\n"
                   "label = \"Rockman\"\n"
                   "\n[[option]]\n"
                   "id = \"script\"\n"
                   "label = \"Script\"\n"
                   "type = \"choice\"\n"
                   "default = \"original\"\n"
                   "[[option.choice]]\n"
                   "value = \"original\"\n"
                   "label = \"Original\"\n"
                   "[[option.choice]]\n"
                   "value = \"retranslation\"\n"
                   "label = \"Retranslation\"\n"
                   "\n[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/matrix.xdelta3\"\n"
                   "patch_sha256 = \"2222222222222222222222222222222222222222222222222222222222222222\"\n"
                   "output_size = 222222\n"
                   "output_sha256 = \"3333333333333333333333333333333333333333333333333333333333333333\"\n"
                   "when = { title = \"rockman\", script = \"retranslation\" }\n"));
    write_text(root / "packages/matrix.mod/1.0.0/assets/matrix.xdelta3", "test");
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", false, &error), error.c_str());
    check(reload.set_enabled("conflict.b", false, &error), error.c_str());
    check(reload.set_enabled("matrix.mod", true, &error), error.c_str());
    check(reload.set_option("matrix.mod", "title", "rockman", &error), error.c_str());
    check(reload.set_option("matrix.mod", "script", "retranslation", &error), error.c_str());
    ModResolution matrix = reload.resolve("SLUS-TEST");
    check(matrix.ok && matrix.derived_discs.size() == 1 &&
              matrix.derived_discs[0].output_size == 222222,
          "multi-option derived-disc condition must match selected values");

    write_text(root / "packages/features.mod/1.0.0/manifest.toml",
               manifest("features.mod", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"title-screen\"\n"
                   "name = \"Title Screen\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"retranslation\"\n"
                   "name = \"Retranslation\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-collision\"\n"
                   "name = \"Title Collision\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-identical\"\n"
                   "name = \"Title Identical\"\n"
                   "\n[[option]]\n"
                   "feature = \"title-screen\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"choice\"\n"
                   "default = \"usa\"\n"
                   "[[option.choice]]\n"
                   "value = \"usa\"\n"
                   "label = \"Mega Man X6\"\n"
                   "[[option.choice]]\n"
                   "value = \"japan\"\n"
                   "label = \"Rockman X6\"\n"
                   "\n[[option]]\n"
                   "feature = \"retranslation\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"boolean\"\n"
                   "default = \"true\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-screen\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"
                   "when = { variant = \"japan\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"retranslation\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 23520\n"
                   "expected = \"03\"\n"
                   "replace = \"b3\"\n"
                   "when = { variant = \"true\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"title-collision\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"02\"\n"
                   "replace = \"ff\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-identical\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"));
    check(reload.scan(&error), error.c_str());
    check(!reload.set_enabled("features.mod", true, &error),
          "feature-style package must not expose package enablement");
    check(reload.set_feature_option(
              "features.mod", "title-screen", "variant", "japan", &error),
          error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-screen", true, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "retranslation", true, &error), error.c_str());
    ModResolution features = reload.resolve("SLUS-TEST");
    check(features.ok && features.writes.size() == 2,
          "independently enabled features must compose their operations");
    check(features.ok && features.writes[0].feature_id == "title-screen" &&
              features.writes[1].feature_id == "retranslation",
          "resolved writes must retain feature ownership");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", true, &error), error.c_str());
    ModResolution collision = reload.resolve("SLUS-TEST");
    check(!collision.ok && collision.diagnostics.size() == 1,
          "overlapping feature writes must produce a structured diagnostic");
    check(!collision.diagnostics.empty() &&
              collision.diagnostics[0].feature_id == "title-collision" &&
              collision.diagnostics[0].other_feature_id == "title-screen" &&
              !collision.diagnostics[0].resource.empty(),
          "collision diagnostic must identify both features and the resource");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", false, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-identical", true, &error), error.c_str());
    ModResolution identical = reload.resolve("SLUS-TEST");
    check(identical.ok && identical.writes.size() == 2,
          "truly identical writes must coalesce deterministically");
    check(reload.save_state(&error), error.c_str());
    ModPackageManager feature_reload(root);
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.load_state(&error), error.c_str());
    check(feature_reload.feature_enabled("features.mod", "title-screen") &&
              feature_reload.feature_enabled("features.mod", "retranslation") &&
              !feature_reload.feature_enabled("features.mod", "title-collision"),
          "per-feature enabled state must survive save/reload");
    check(feature_reload.feature_option_value(
              "features.mod", "title-screen", "variant") == "japan",
          "feature-scoped option values must survive save/reload");
    check(feature_reload.resolve("SLUS-TEST").fingerprint ==
              identical.fingerprint,
          "feature state must resolve deterministically after reload");

    const std::vector<uint8_t> overlay_a = {1, 2, 3, 4};
    const std::vector<uint8_t> overlay_b = {8, 9};
    const std::string overlay_disc_hash(64, '4');
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/a.bin", overlay_a);
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/b.bin", overlay_b);
    write_text(root / "packages/overlay.mod/1.0.0/manifest.toml",
               manifest("overlay.mod", "1.0.0",
                   "disc_sha256 = \"" + overlay_disc_hash + "\"\n"
                   "[[feature]]\n"
                   "id = \"asset-a\"\n"
                   "name = \"Asset A\"\n"
                   "[[feature]]\n"
                   "id = \"asset-b\"\n"
                   "name = \"Asset B\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-a\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 100\n"
                   "file = \"assets/a.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_a) + "\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-b\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 102\n"
                   "file = \"assets/b.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_b) + "\"\n"));
    check(feature_reload.scan(&error), error.c_str());
    const ModPackage* overlay_package =
        feature_reload.selected_package("overlay.mod");
    check(overlay_package && overlay_package->overlays.size() == 2 &&
              overlay_package->overlays[0].size == overlay_a.size(),
          "manifest scan must verify and retain overlay metadata");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-a", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", true, &error), error.c_str());
    ModResolution overlay_collision =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(!overlay_collision.ok &&
              overlay_collision.diagnostics.size() == 1 &&
              overlay_collision.diagnostics[0].feature_id == "asset-b" &&
              overlay_collision.diagnostics[0].other_feature_id == "asset-a",
          "overlapping file overlays must identify both owning features");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", false, &error), error.c_str());
    ModResolution one_overlay =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(one_overlay.ok && one_overlay.overlays.size() == 1 &&
              one_overlay.overlays[0].payload == overlay_a,
          "only enabled overlay payloads must enter the resolved plan");

    write_text(root / "feature-derived.toml",
               manifest("bad.derived", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"bad\"\n"
                   "name = \"Bad\"\n"
                   "[[derived_disc]]\n"
                   "patch = \"bad.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 1\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"));
    ModPackage feature_derived;
    check(!ModPackageManager::read_manifest(
              root / "feature-derived.toml", feature_derived, &error),
          "feature-style packages must reject derived-disc operations");

    ModPackage invalid;
    write_text(root / "bad.toml",
               "format_version=1\nid=\"../bad\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
               "[[target]]\ngame_id=\"SLUS-TEST\"\n");
    check(!ModPackageManager::read_manifest(root / "bad.toml", invalid, &error),
          "unsafe package id must be rejected");

    fs::remove_all(root, ec);
    if (failures) {
        std::cerr << failures << " mod package test(s) failed\n";
        return 1;
    }
    std::cout << "mod package tests passed\n";
    return 0;
}
