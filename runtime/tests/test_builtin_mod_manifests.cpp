/* Every framework-builtin mod package must parse under the current manifest
 * schema. A builtin that fails to parse is silently dropped from the catalog
 * at runtime ("mod manifest ignored"), which for an activation package such as
 * psx.enhancement.8mb-ram makes the feature impossible to enable while every
 * other test stays green.
 *
 * Usage: builtin_mod_manifests_test <mods/builtin/packages dir> */
#include "mod_packages.h"
#include "host_enhancements.h"
#include "internal_resolution.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static int failures;

static void check(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: builtin_mod_manifests_test <packages dir>\n";
        return 2;
    }
    const fs::path root(argv[1]);
    int parsed = 0;
    bool saw_8mb = false;
    /* The host enhancements that used to be Settings rows (beads-eio.3.398):
     * package id -> {feature id, plugin id, option id -> default}. Each is
     * default-off, targets every game, and is host-only (netplay =
     * "host_only"): the player's own in a match, never in the fingerprint. */
    struct HostEnhancement {
        std::string feature, plugin;
        std::map<std::string, std::string> defaults;
        bool seen = false;
    };
    std::map<std::string, HostEnhancement> host_enhancements = {
        {"psx.enhancement.crt-filter", {"crt-filter", "psx.crt-filter",
            {{"screen_model", "default"}, {"scanlines", "default"},
             {"scanline_strength", "50"}}}},
        {"psx.enhancement.image-filtering", {"image-filtering", "psx.image-filtering",
            {{"smoothing", "default"}, {"texture_filtering", "default"},
             {"fmv_filtering", "default"}}}},
        {"psx.enhancement.enhanced-audio", {"enhanced-audio", "psx.enhanced-audio", {}}},
        {"psx.feature.rewind", {"rewind", "psx.rewind",
            {{"buffer", "50"}, {"interval", "15"}}}},
        {"psx.enhancement.internal-resolution", {"internal-resolution",
            "psx.internal-resolution", {{"resolution", "default"}}}},
    };
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().filename() != "manifest.toml")
            continue;
        ModPackage package;
        std::string error;
        const bool ok = ModPackageManager::read_manifest(entry.path(), package, &error);
        check(ok, entry.path().string() + ": " + error);
        if (!ok) continue;
        parsed++;
        const fs::path version_dir = entry.path().parent_path();
        check(package.version == version_dir.filename().string(),
              entry.path().string() + ": version does not match its directory");
        check(package.id == version_dir.parent_path().filename().string(),
              entry.path().string() + ": id does not match its directory");
        if (package.id == "psx.enhancement.8mb-ram") {
            saw_8mb = true;
            check(package.features.size() == 1 && package.features[0].id == "8mb-ram",
                  "8 MB package exposes exactly the 8mb-ram feature");
            check(!package.features.empty() && !package.features[0].default_enabled,
                  "8 MB RAM is opt-in (default disabled)");
            check(!package.features.empty() && package.features[0].hidden,
                  "8 MB RAM is hidden by default");
            check(package.plugins.size() == 1 && package.plugins[0].id == "psx.8mb-ram" &&
                      package.plugins[0].feature_id == "8mb-ram",
                  "8 MB feature activates the psx.8mb-ram plugin");
            check(package.targets.size() == 1 && package.targets[0].game_id == "*",
                  "8 MB package applies to every game");
        }
        auto he = host_enhancements.find(package.id);
        if (he != host_enhancements.end()) {
            HostEnhancement& want = he->second;
            want.seen = true;
            const std::string who = package.id + ": ";
            check(package.features.size() == 1 && package.features[0].id == want.feature,
                  who + "exposes exactly its one feature");
            check(!package.features.empty() && !package.features[0].default_enabled,
                  who + "is default-off (the title's baseline applies)");
            check(!package.features.empty() && package.features[0].host_only,
                  who + "is host-only");
            check(package.targets.size() == 1 && package.targets[0].game_id == "*",
                  who + "applies to every game");
            check(package.plugins.size() == 1 && package.plugins[0].id == want.plugin &&
                      package.plugins[0].netplay_host_only,
                  who + "activates its host_only plugin");
            check(package.save_compatibility == "shared",
                  who + "never isolates save states");
            check(package.options.size() == want.defaults.size(),
                  who + "declares exactly its documented options");
            for (const ModOption& option : package.options) {
                auto d = want.defaults.find(option.id);
                check(d != want.defaults.end() && option.default_value == d->second,
                      who + option.id + " defaults to its documented value");
            }
        }
        /* Internal Resolution offers Game default and exactly the runtime's
         * preset vocabulary (internal_resolution.h), Auto included, and every
         * choice parses to the value the runtime resolves. */
        if (package.id == "psx.enhancement.internal-resolution" && package.options.size() == 1) {
            const ModOption& option = package.options[0];
            check(option.choices.size() == (size_t)PSX_IR_PRESET_COUNT + 1,
                  "Internal Resolution offers Game default plus every preset");
            check(!option.choices.empty() && option.choices[0].value == "default" &&
                      option.choices[0].label == "Game default",
                  "Internal Resolution's first choice is Game default");
            for (int i = 0; i < PSX_IR_PRESET_COUNT && i + 1 < (int)option.choices.size(); ++i) {
                const ModChoice& c = option.choices[i + 1];
                check(c.value == k_psx_ir_presets[i].id && c.label == k_psx_ir_presets[i].label,
                      "Internal Resolution choice " + c.value + " matches the preset table");
                check(psx_host_enh_parse_internal_resolution(c.value.c_str()) ==
                          k_psx_ir_presets[i].value,
                      "Internal Resolution choice " + c.value + " parses to its value");
            }
        }
    }
    check(parsed > 0, "no builtin manifests found under " + root.string());
    check(saw_8mb, "psx.enhancement.8mb-ram builtin package missing");
    for (const auto& [id, want] : host_enhancements)
        check(want.seen, id + " builtin package missing");
    if (failures == 0)
        std::cout << "PASS builtin mod manifests (" << parsed << " packages)\n";
    return failures ? 1 : 0;
}
