/*
 * Framework-owned scene capture mod, available to every game.
 *
 * mods/builtin/packages/psx.framework.scene-capture targets game_id "*".
 * Default off; enabling it turns on the observer in scene_capture.cpp. The
 * per-game hooks come from scene_adapter.toml (docs/scene_capture.md), which
 * main.cpp loads whether or not the mod is on: the hook callbacks do nothing
 * while capture is off.
 */
#include "mod_plugins.h"
#include "scene_capture.h"

static void builtin_scene_capture_activate(void) {
    scene_capture_set_enabled(1);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_scene_capture_plugin) {
    (void)psx_mod_register_activation_plugin("psx.scene_capture.enable",
                                             builtin_scene_capture_activate);
}
