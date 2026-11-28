/* Per-game scene adapter (docs/scene_capture.md): a small TOML next to
 * game.toml that sets the link window and names the guest addresses of the
 * libgte / libgpu entry points to hook. Addresses come from
 * tools/psyq_sigs output; each must also be listed in game.toml
 * [recompiler] mod_function_entry_funcs so the generated code calls the
 * mod entry hook. The callbacks only read a0. */

#include "scene_capture.h"
#include "cpu_state.h"
#include "mod_plugins.h"
#include "toml.hpp"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void entry_rot(CPUState *cpu, uint32_t) {
    if (g_scene_capture_on) scene_capture_note_rot_matrix(cpu->gpr[4]);
}
void entry_trans(CPUState *cpu, uint32_t) {
    if (g_scene_capture_on) scene_capture_note_trans_matrix(cpu->gpr[4]);
}
void entry_draw(CPUState *cpu, uint32_t address) {
    if (g_scene_capture_on) scene_capture_note_draw_call(address, cpu->gpr[4]);
}

std::string g_adapter_error;

/* The registry has no removal, so each address is registered once with this
 * dispatcher and the current adapter's map decides what it does. Reloading an
 * adapter replaces the map; an address it no longer lists does nothing. */
std::unordered_map<uint32_t, PSXModFunctionEntryCallback> g_hook_kind;
std::unordered_set<uint32_t> g_registered;

void entry_dispatch(CPUState *cpu, uint32_t address) {
    const auto it = g_hook_kind.find(address);
    if (it != g_hook_kind.end()) it->second(cpu, address);
}

}  // namespace

extern "C" const char *scene_capture_adapter_error(void) { return g_adapter_error.c_str(); }

extern "C" int scene_capture_load_adapter(const char *path, char *err, uint32_t err_size) {
    auto fail = [&](const std::string &m) {
        g_adapter_error = m;
        if (err && err_size) std::snprintf(err, err_size, "%s", m.c_str());
        return 0;
    };
    if (!path || !*path) return fail("no adapter path");
    try {
        const toml::value cfg = toml::parse(std::string(path));
        if (toml::find_or<int64_t>(cfg, "format", 0) != 1) return fail("adapter format must be 1");
        const std::string game = toml::find_or<std::string>(cfg, "game", "");
        const int64_t window = toml::find_or<int64_t>(cfg, "link_window_vblanks", 4);
        if (window < 1 || window > 60) return fail("link_window_vblanks must be 1..60");
        std::vector<std::pair<uint32_t, PSXModFunctionEntryCallback>> hooks;
        if (cfg.contains("hooks")) {
            const toml::value &h = toml::find(cfg, "hooks");
            const std::pair<const char *, PSXModFunctionEntryCallback> kinds[] = {
                {"draw_ot", entry_draw},
                {"set_rot_matrix", entry_rot},
                {"set_trans_matrix", entry_trans},
            };
            for (const auto &k : kinds) {
                if (!h.contains(k.first)) continue;
                for (const toml::value &a : toml::find(h, k.first).as_array()) {
                    const int64_t addr = a.as_integer();
                    if (addr <= 0 || addr > 0xFFFFFFFFll)
                        return fail(std::string("bad address in hooks.") + k.first);
                    for (const auto &seen : hooks)
                        if (seen.first == (uint32_t)addr)
                            return fail(std::string("address listed twice in hooks: ") + k.first);
                    hooks.push_back({(uint32_t)addr, k.second});
                }
            }
        }
        for (const auto &hk : hooks) {
            if (g_registered.count(hk.first)) continue;
            if (!psx_mod_register_function_entry_plugin("psx.scene_capture", hk.first, entry_dispatch))
                return fail("could not register a hook address");
            g_registered.insert(hk.first);
        }
        g_hook_kind.clear();
        for (const auto &hk : hooks) g_hook_kind[hk.first] = hk.second;
        g_adapter_error.clear();
        scene_capture_set_adapter_game(game.c_str());
        scene_capture_set_link_window((uint32_t)window);
        return 1;
    } catch (const std::exception &e) {
        return fail(e.what());
    }
}
