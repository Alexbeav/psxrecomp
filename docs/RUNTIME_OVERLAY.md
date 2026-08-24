# Runtime overlay and live-apply contract

The PSX host uses recomp-ui's `RecompRuntimeUi` model and standard setting
catalog when `RECOMP_LAUNCHER` is available.  Its renderer-independent
ARGB8888 presentation is composited into the existing 640x480 host overlay
plane, so the same menu works with the software, OpenGL, and Vulkan presenters.
Builds without recomp-ui retain the legacy controller-route-only panel.

The host pauses guest execution at a VBlank boundary while the menu is open.
Callbacks therefore cannot race guest instructions, SIO transfers, or CD-ROM
commands.  F1 opens the menu; keyboard arrows/Enter/Escape and controller
D-pad/Cross/Circle navigate it.  F6 remains the direct controller-route
shortcut.

## Apply tiers

Every setting belongs to one tier.  A control must not be shown as live until
its complete tier transition exists for every enabled renderer.

| Tier | Current examples | Rule |
| --- | --- | --- |
| Live | fullscreen, window size, final-image filter, texture filter, volume, controller route | Apply immediately; persist normal preferences. Controller routing is intentionally session-only. |
| Renderer recreate | supersampling/internal resolution, renderer, MSAA/render-target format | Pause, drain the presenter, recreate all backend-owned targets, then commit the new value. On failure restore the old backend and value. |
| Runtime restart | BIOS image, startup disc, netplay identity/topology | Do not mutate a running guest. Save the preference and report that a restart is required. |
| Mod owned | widescreen/view mode, FMV skipping, load acceleration | The trusted mod activation plan is authoritative. Do not add a second generic control that can overwrite it. |

Supersampling is specifically **not** exposed by the in-game menu yet.  A bare
assignment to `g_video_scale` or a backend scale setter is insufficient: the
software staging texture, OpenGL high-resolution FBOs, Vulkan targets, logical
present size, and netplay CPU-authority rules must transition as one operation.
Until that transaction exists and is tested, the launcher/startup setting is
the only supported path.

## Disc actions

`Change disc...` resolves a picked BIN/ISO/IMG/CAR/CUE/CHD through the shared
disc-path resolver, opens the replacement before releasing the current handle,
updates the inserted-disc SCE region when identifiable, resets active data,
CD-DA, XA and subchannel state, and emits the existing reinsert notification.
An unreadable selection leaves the current disc mounted.

The selected path is session-only: it is not written to `settings.toml` or
`disc.cfg`, because a game-requested Disc 2 or a foreign audio CD must not
become the next startup disc.  Unknown-region/audio discs retain the previous
SCE response.  Disc replacement is disabled during netplay.  `Reinsert current
disc` remains available for games that ask the player to re-close the tray
without changing media.

Acceptance testing should cover:

- a conventional multi-disc prompt (for example Syphon Filter 2 Disc 2);
- Vib-Ribbon reinsertion and, separately, an audio-CD swap;
- a cancelled or invalid picker selection preserving the running disc;
- region changes between identifiable PSX discs;
- software, OpenGL, and Vulkan overlay presentation.

The drive currently reports the close/reinsert transition immediately, as the
pre-existing debug action did.  If a title requires guest-observable tray-open
dwell time, add it as title-neutral CD-controller state with tests; do not add a
per-title delay.
