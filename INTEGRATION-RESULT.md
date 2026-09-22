# SF2 launcher integration — RESULT (2026-09-22)

**Status: the merge builds and launches.** `EXIT=0` on configure and build.

## What was produced

A single runtime carrying **both** feature sets at once, which neither parent had:

| Symbol present in the linked binary | Feature | Source line |
| --- | --- | --- |
| `host_keymap_down`, `host_keymap_load`, `host_keymap_match`, `host_keymap_match_event` | **F1 / F7 menus** | launcher baseline (`f4751d00`) |
| `psx_mouse_camera_hook`, `psx_mouse_camera_configure`, `psx_mouse_camera_commit_frame`, `psx_mod_set_mouse_camera` | **mouse-look** | SF2 branch (`452cc0c`) |
| `psx_rewind_*` (17 symbols) | **Rewind** | launcher baseline + rbengine |
| `recomp_launcher_run_window`, `launcher_imgui.cpp` | **recomp-ui launcher** | paired recomp-ui `ff92028` |

- exe: `I:\Projects\_local\integration-kit\bld\Syphon_Filter_2_Recompiled.exe`
- 47,596,984 bytes, SHA-256 `97282fb89d1a0420572baad527dc16f71539fee9a69d323ef2415ab197901bff`
- Launches; stderr is `main() entered` only (clean argv)
- Configure gates: `BIOS backends linked: SCPH1001`, `rewind snap_ring (.../retcomm-rbengine)`, `recomp-ui: SDL3 platform backend`

## Dependency set actually required (discovered, not assumed)

The framework enforces pairings by compile-time gates. All three had to move together:

| Component | Ref | Why |
| --- | --- | --- |
| psxrecomp | `f4751d00` + `452cc0c` merged | feature union |
| recomp-ui | `ff92028ec86e30503694c70c532b93b8198663aa` | `runtime.cmake:2022` requires `ff92028` or newer for `RecompLauncherCNetplayChatMessage` |
| retcomm-rbengine | `a7b98507a62fe00e5aec3b90c52a4134f3c174bc` | `runtime.cmake:559` requires the snap-ring backend when `PSX_REWIND=ON` |
| recomp-net | `268e74f` | unchanged |

**recomp-ui note:** `ff92028` is the tip of branch `wave4-posix-tools-20260914` and is **not** an ancestor of `Alexbeav/recomp-ui` `main` (verified: `merge-base --is-ancestor` fails, and `main` does not contain `RecompLauncherCNetplayChatMessage`). The kit's previous `514c9e29` is too old for this framework.

**rbengine note:** `a7b98507` is `refs/heads/main` of `RetroPortingToolKit/rbengine`, recorded as the submodule gitlink in the framework tree. A GitHub source ZIP never contains it, which is why the previous kit had no `lib/retcomm-rbengine`.

## Merge shape

- 119 files on the SF2 side, 1173 on the launcher side, 42 overlapping
- **111 auto-merged**; **25 conflicted** across 97 hunks
- Resolution policy and per-file inventory: `INTEGRATION-conflicts.md` in the merge tree
- Two automated resolvers were used, both of which refuse anything they cannot prove safe:
  - `resolve-union.ps1` — union of `#include`-only hunks (guarded)
  - `resolve-takeours.ps1` — takes ours only where the theirs-side matches a supplied regex

## Real defects found by compiling (not by reading)

Compilation caught five errors that conflict-marker checks could not. Each is a case
where a resolution looked fine in one file but broke consistency elsewhere.

1. **`gpu.c` — orphaned `}`** (+1). Leftover tail of the original hunk after a union.
2. **`gpu.c` — `ws_nw_compensate_triangle` lost.** SF2 defines it; my "take ours" on the
   PGXP hunks removed it while 4 call sites remained. Restored, with a forward
   declaration of `parse_vertex` because it must precede that function.
3. **`debug_server.c` — missing `}`** in `handle_savestate_status`; SF2's
   `handle_mouse_camera_input`/`_stats` had been spliced inside it.
4. **`main.cpp` — `rui_keybinds_path` out of scope.** SF2 scoped it locally; the baseline
   uses global `g_rui_keybinds_path`. Switched to the global, with an empty-guard.
5. **`dma.c` — `gpu_ws_begin_linked_list()` arity.** SF2's version takes `start_addr`
   (needed for linked-list world detection) and the merged `gpu.h` declares that form;
   `dma.c` had been resolved to the baseline's 0-arg call. Both call sites updated, and
   5 test stubs corrected to the new signature.

Lesson recorded: **brace-balance and marker greps are not sufficient.** All five were
found only by running the compiler.

## Verification performed

| Check | Result |
| --- | --- |
| cmake configure | EXIT=0 |
| build `psx-runtime` | EXIT=0, 256/256 |
| exe produced | 47,596,984 bytes |
| launches | yes, window up, clean argv |
| F1/F7 symbols linked | yes (`host_keymap_*`) |
| mouse-look symbols linked | yes (`psx_mouse_camera_*`) |
| rewind symbols linked | yes (`psx_rewind_*`) |
| BIOS gate | `BIOS backends linked: SCPH1001` |

## NOT verified — this is a build result, not an acceptance result

Compiling and launching prove nothing about behaviour. Each still needs its own test,
reported separately:

1. **F1 menu** opens and is usable
2. **F7 save-state menu** opens and is usable
3. **Mouse-look** behaves correctly in game
4. **Widescreen** visually widens; culling fills edges; HUD/backdrop placement
5. **Controllers** recognised; binds work
6. **Memory cards** create/save/load
7. **Disc changes** Disc 1 → Disc 2
8. **Rewind** engages without breaking savestates
9. Each optional feature tested **off and on**
10. Distribution validated through clean `SETUP` + launcher installation

## Also unresolved

- `game.toml` in the integration kit still points at `input/` for `exe`/`disc`, while the
  framework now stages `bld/mods`. Reconcile before packaging.
- The merged framework is a **local merge, not a published revision**. Publishing requires
  a new ref, a minted archive SHA-256, and a `SETUP.ps1` update — none done.
- 4 tests the earlier resolver registered fail on the merged baseline for cross-file
  reasons (`test_cdrom_seek_retarget.py`, `test_gl_depth24_coherency.py`,
  `test_host_input_focus_guards.py`, `test_overlay_dump_bounds.py`). Not yet fixed.

## Preserved

- `sf2-widescreen-candidate` kit — frozen as the widescreen comparison candidate
- `FROZEN-working-build` — SHA-256 `2c46e4430401ac9fa299448e49527aee9567ab2a33a89b0b62802174bccd687f`, unchanged
- No commits or pushes in any repository

