# SF2 integration — conflict inventory (2026-09-22)

Branch `integration-test` in `I:\Projects\_local\psxrecomp-merge`
= `f4751d00` (launcher/wave5 baseline) + trial merge of `452cc0c` (SF2 feature branch).

97 conflict hunks across 25 files. Only **4** are safely mechanical
(include-only union, resolved by `resolve-union.ps1`). The remaining **92**
require engineering judgement and are listed below with their resolution policy.

`resolve-union.ps1` deliberately refuses any hunk containing real code, so a
semantic conflict can never be unioned into a broken merge. Refused hunks keep
the launcher-side text with a visible marker for review.

## Files and policy

| File | Hunks | Policy |
| --- | --- | --- |
| `runtime/src/gpu.c` | 22 | **Hard.** Mixed: some additive (WS modules), some signature changes (`ws_expand_fullscreen_rect` differs in signature AND return type). Per-hunk review. |
| `runtime/src/main.cpp` | 20 | **Hard.** Widescreen projection refresh (`g_ws_projection_mode` vs `g_video_aspect_num/den`), PGXP generations, input-frame finalisation ordering, launcher/CLI additions to KEEP. |
| `runtime/src/psx_keybinds.c` | 13 | **Medium.** Both sides added mouse pseudo-scancodes; conflicts are comments + wrapping. Launcher side adds `s_alt_binds`. Take launcher structure, keep SF2 semantics. |
| `runtime/src/debug_server.c` | 6 | Medium. |
| `runtime/src/gpu_gl_renderer.c` | 5 | **Hard.** Render path. |
| `recompiler/src/code_generator.cpp` | 4 | **Hard.** Emits the hook and load-delay pairs. |
| `runtime/src/overlay_loader.c` | 3 | Medium. |
| `recompiler/CMakeLists.txt` | 2 | Easy (probe paths). |
| `recompiler/src/config_loader.h` | 2 | **Hard.** Two PGXP API generations. |
| `recompiler/tests/recompiler_patch_test.cpp` | 2 | Easy. |
| `runtime/src/cdrom.c` | 2 | Medium (seek/probe counters). |
| `runtime/src/gpu_render.c` | 2 | Medium. |
| `runtime/src/gte.cpp` | 2 | **Hard.** PGXP API generations + NCLIP precise. |
| `runtime/CMakeLists.txt` | 1 | Easy — union of both test-registration blocks (files exist on both sides). |
| `runtime/include/cpu_state.h` | 1 | Easy — union (both declare different APIs). |
| `runtime/include/gpu.h` | 1 | Easy. |
| `runtime/include/gpu_render.h` | 1 | Easy. |
| `runtime/include/psx_keybinds.h` | 1 | Easy — `psx_keybinds_get_button_alt` from launcher side. |
| `runtime/src/crash_trace.c` | 1 | Easy. |
| `runtime/src/dirty_ram_interp.c` | 1 | Medium. |
| `runtime/src/dma.c` | 1 | Easy. |
| `runtime/tests/test_overlay_call_unit_irq_guards.py` | 1 | add/add — merge both test sets. |
| `tools/build_cli.py` | 1 | Easy. |
| `tools/compile_overlays.py` | 1 | Medium. |
| `tools/test_compile_overlays_additive.py` | modify/delete | Deleted on launcher side, modified on SF2 side. Decide whether the test is still meaningful. |

## Resolution policy (unchanged)

1. Launcher baseline structure for shared infrastructure (keybinds persistence,
   CLI parsing, launcher UI, netplay, input timing).
2. SF2 feature layer wholesale: `mouse_camera.c/h`, `ws_projection_compose.h`,
   `ws_fullwidth_effect.h`, `ws_backdrop_owner.h`, `mouse_pad_adapter.c/h`,
   `pad_timeline.cpp/h` + tests. These auto-merged.
3. Where one feature exists on both sides, keep **one** implementation — prefer
   whichever the newer code still calls.
4. Keep F1/F7 (`host_keymap.c`) and wire SF2's mouse-look/widescreen into it.
5. Nothing dropped silently.

## Critical detail found during inventory

`ws_expand_fullscreen_rect` in `gpu.c` differs between branches:

```c
/* launcher side */
static void ws_expand_fullscreen_rect(int32_t *x, int32_t y, int *w, int h)

/* SF2 side */
static int ws_expand_fullscreen_rect(int32_t *x, int32_t y, int *w, int h,
                                     int32_t authored_left)
```

Different return type (void vs int) and an extra parameter. Union is impossible;
this needs a deliberate choice and every call site updated. Same class of
problem likely in the `gte.cpp` / `config_loader.h` PGXP API pair.
