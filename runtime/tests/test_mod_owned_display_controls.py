#!/usr/bin/env python3
"""Guard PSX display enhancements as trusted-mod-only features."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)

for declaration in (
    "constexpr bool ws_offered = false;",
    "constexpr bool ws_ultrawide_offered = false;",
    "constexpr bool frame_interpolation_offered = false;",
    "constexpr bool skip_fmv_offered = false;",
):
    assert declaration in MAIN, f"PSX launcher capability must default off: {declaration}"

for legacy_route in (
    "ws_offered = gc.ws_offered;",
    "ws_ultrawide_offered = gc.ws_ultrawide_offered;",
    "frame_interpolation_offered =\n                gc.runtime.video_offer_frame_interpolation;",
    "skip_fmv_offered = gc.runtime.video_offer_skip_fmv;",
):
    assert legacy_route not in MAIN, f"legacy offer flag still controls UI: {legacy_route}"

for hidden_capability in (
    "gi->widescreen_supported = 0;",
    "gi->aspect_mask = 0;",
):
    assert hidden_capability in MAIN

for trusted_api in (
    "psx_mod_set_fixed_display_aspect",
    "psx_mod_set_adaptive_display_aspect",
    "psx_mod_set_frame_interpolation",
    "psx_mod_set_auto_skip_fmv",
):
    assert trusted_api in HEADER, f"missing trusted mod API: {trusted_api}"

assert MAIN.index("g_auto_skip_fmv = 0;") < MAIN.index("mod_runtime_activate_plugins();")

# Session hygiene: mod-owned presentation state must not outlive the session
# that set it. The one in-process second session is the lobby rematch after a
# netplay match; it jumps past the first-boot block, so the reset runs on both
# paths. The reachable leak is the netplay local viewport's Fit/aspect into an
# offline rematch; the rest of the list is defensive. The first-call capture
# itself is exercised by mod_session_baseline_test (behavioural); this guard
# pins main.cpp to that helper and to the call sites.
helper_start = MAIN.index("static void reset_mod_owned_presentation(void) {")
helper = MAIN[helper_start:MAIN.index("\n}\n", helper_start)]
assert '#include "mod_session_baseline.h"' in MAIN
assert "psx_mod_session_baseline_apply(" in helper, \
    "reset must go through the tested first-call capture helper"
for reset in (
    "g_video_vsync = live.video_vsync;",
    "g_frame_interpolation = live.frame_interpolation;",
    "g_frame_interpolation_fps = live.frame_interpolation_fps;",
    "g_auto_skip_fmv = live.auto_skip_fmv;",
    "g_guest_frame_period_ms = live.guest_frame_period_ms;",
    "g_frame_period_ms = live.frame_period_ms;",
    "g_mod_native_vblank_rate = false;",
    "g_ws_adaptive_view = false;",
    "psx_mod_set_world_scene_predicate(nullptr);",
    "psx_mod_set_retained_scene_predicate(nullptr);",
    "psx_mod_set_adaptive_backdrop_preload(0);",
    "g_bezel_path.clear();",
    "g_frame_interpolation_blend = g_frame_interpolation_blend_default;",
):
    assert reset in helper, f"mod-owned session reset is missing: {reset}"

# Later calls only (the first call changes nothing that is not already at its
# initial value): the 8 MiB RAM request, which memory_init() re-latches at
# session_reboot, and the texture-bank resolver/batching flag.
later = helper[helper.index("if (!first) {"):]
later = later[:later.index("}")]
for reset in (
    "psx_ram_reset_size_request();",
    "psx_mod_set_texture_bank_resolver(nullptr);",
    "psx_mod_set_texture_bank_batching(0);",
):
    assert reset in later, f"later-session reset is missing: {reset}"

# Bezel artwork: clearing g_bezel_path only stops the next load. The loaded
# texture must not outlive the session's GL context either, or a rematch would
# bind the stale name in its new context (present_bezel() draws whenever it is
# nonzero). Only a soft return and process exit shut the renderer down.
GL = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(encoding="utf-8")
shutdown = GL[GL.index("void gl_renderer_shutdown(void) {"):]
shutdown = shutdown[:shutdown.index("\n}\n")]
live_ctx = shutdown[shutdown.index("if (s_ctx) {"):shutdown.index("SDL_GL_DeleteContext(s_ctx);")]
assert "gl_renderer_set_bezel(NULL, 0, 0);" in live_ctx, \
    "gl_renderer_shutdown() must drop the bezel texture while its context is current"
set_bezel = GL[GL.index("int gl_renderer_set_bezel(const void *rgba, int w, int h) {"):]
assert set_bezel.index("if (s_bezel_tex) { glDeleteTextures(1, &s_bezel_tex); s_bezel_tex = 0; }") < \
    set_bezel.index("if (!rgba || w <= 0 || h <= 0) return 1;"), \
    "gl_renderer_set_bezel(NULL, ...) must delete and forget the texture"
assert "if (!s_bezel_tex || ww <= 0 || wh <= 0) return;" in GL

# First-boot session block: reset immediately before activation.
assert "reset_mod_owned_presentation();\n    mod_runtime_activate_plugins();" in MAIN, \
    "reset must run immediately before mod activation"

# Soft return (rematch) re-enters below that block via `goto session_reboot`,
# so the rematch path must reset too, after its commit / netplay clear.
rematch = MAIN[MAIN.index('"psxrecomp: cannot clear mods for netplay "'):]
rematch = rematch[:rematch.index("goto session_reboot;")]
commit = rematch.index("mod_runtime_commit(resolved_disc,")
reset_at = rematch.index("reset_mod_owned_presentation();")
viewport = rematch.index("apply_netplay_local_viewport_aspect(net_cfg.enabled);")
assert commit < reset_at < viewport, \
    "rematch must reset mod-owned state after its commit, before netplay aspect"
# If the rematch path ever activates plugins, the reset must come first, or it
# would undo what activation just set.
if "mod_runtime_activate_plugins();" in rematch:
    assert reset_at < rematch.index("mod_runtime_activate_plugins();"), \
        "rematch must reset mod-owned state before activation"

# Rematch 4:3 re-clamp: the launcher round-trips the previous match's aspect
# (16:9/21:9 from the netplay local viewport) through ls.aspect_index. While
# widescreen is mod-owned the Settings aspect is 4:3, so the rematch path
# re-applies that clamp after the launcher, before the local viewport.
clamp = "if (!ws_offered) {\n                g_video_aspect_num = 4;\n                g_video_aspect_den = 3;\n            }"
assert rematch.count(clamp) == 1, "rematch must re-clamp the aspect to 4:3"
clamp_at = rematch.index(clamp)
assert commit < clamp_at < viewport, \
    "4:3 re-clamp must follow the commit and precede the netplay local viewport"
rematch_start = MAIN.index('"psxrecomp: cannot clear mods for netplay "')
assert MAIN.rfind("case 1:  g_video_aspect_num = 16; g_video_aspect_den = 9; break;",
                  0, rematch_start) > MAIN.index("soft_return_lobby:"), \
    "the launcher's aspect must be applied before the rematch re-clamp"

print("mod-owned PSX display controls guard passed")
