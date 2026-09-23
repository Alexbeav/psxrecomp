# Repairs and corrections — 2026-09-22

## 1. Input-code defect: the focus guard was genuinely missing (FIXED)

**Correction to my earlier claim.** I first reported this as "SF2-only function
`hybrid_dpad_active` dropped by the merge." That was wrong. The function was
**renamed**, not lost: SF2's `hybrid_dpad_active(p, player, dev_any)` became the
launcher baseline's `controller_policy_dpad_active(p, player, src)` with a
`PadSources` struct. Same logic, same joystick loop, same
`psx_keybinds_dpad_active` call.

**But investigating it found a real defect.** The merged
`controller_policy_dpad_active` had **lost the neutral-input guard** that both its
siblings and SF2's original carry:

| Function | `SDL_WINDOW_INPUT_FOCUS` guard |
| --- | --- |
| `pad_from_keyboard` | yes |
| `pad_sticks_for` | yes |
| `controller_policy_dpad_active` | **no — before fix** |

So the keyboard D-pad path asserted buttons **while the window was hidden,
headless, or unfocused**, leaking input from an inactive window. This is exactly
what `runtime/tests/test_host_input_focus_guards.py` protects.

**Fix** (`runtime/src/main.cpp`, the `src.keybinds` branch): apply the same guard
the siblings use.

**Separate change:** the test was also updated to name the surviving function
(`controller_policy_dpad_active` → `controller_policy_resolve_mode` boundaries),
because it is an SF2-only test (added in `5b64d86c`, absent from `f4751d00`) that
referenced the pre-merge identifier. The behaviour it asserts is unchanged.

Result: `host input focus guards: PASS`.

## 2. Duplicate definition (FIXED)

`runtime/tests/overlay_pair_dedup_harness.c` defined `psx_ws_angle_widen`,
`psx_ws_cull_keep_result`, `psx_ws_aspect_cone_result` **twice** (lines ~146-154
and ~192-199) — the merge concatenated both parents' stub blocks.

**Fix:** removed the second block, keeping one definition with a comment recording
why.

A second, related link failure then surfaced: the merged `overlay_loader.c`
references `psx_cpu_step_boundary_fn` / `psx_cpu_step_boundary_enabled`, which this
harness did not provide. Added the same stubs `overlay_resident_patch_harness.c`
already uses.

Result: `PASS: overlay crash serializers preserve bounds and valid JSON`.

## 3. The other two tests — what they protect, established before changing anything

### `test_gl_depth24_coherency` — still failing

Protects: **the 15-bit → packed-24-bit VRAM handoff**, via an ordered assertion in
`gp1_display_mode`:

```python
"old_display_depth = display_depth",
"display_depth = (val >> 4) & 1",
"gr_display_depth_changed",
```

Investigated: the merged code does **capture the old value before assigning the new**
(`old_display_depth` at line 6257, `display_depth = new_depth` at 6263, then
`gr_display_depth_changed` at 6277) — so the **ordering contract holds**.

It fails on an exact literal string. The expected spelling
(`old_display_depth = display_depth`) exists in **no** revision available here:
`f4751d00`/`main`/`40ce478` have no `old_display_depth` at all, and SF2 uses
`old_display_depth = display_depth & 1u` with a direct `display_depth = (val >> 4) & 1`.
My merge introduced `new_depth` as an intermediate, matching neither.

**Not yet changed.** The semantics appear preserved, but I have not proven the
depth-24 handoff behaves correctly, and this is the same area as the VRAM fault
below. Deferring until the display-source issue is understood, since altering this
to satisfy a string could mask a real ordering problem.

### `test_cdrom_seek_retarget` — still failing

Protects: **CD seek/read retargeting** — that a seek during a read re-targets
rather than completing stale data, with `cdrom_clear_pending_dataready()` called.

Investigated: `cdrom_clear_pending_dataready` appears **9 times** in merged
`cdrom.c` — same as `f4751d00`, more than SF2's 4. My resolver deliberately took the
baseline's `cdrom.c` (whole file is `pending_arm`/source-model based). So the
symbol is present and richer; the test indexes an exact signature/ordering the
baseline words differently.

**Not yet changed.** Same reasoning: a text-shape mismatch, but the behaviour it
encodes has not been verified.

## 4. Where the image is lost (separate document)

`IMAGE-LOSS-LOCATED.md`. Summary: displayed output is **pure black** (sampled from a
`screenshot_file` PNG: 1 distinct colour, 0 non-black pixels); the display area is
`x=0,y=240,320x240`; the guest **is** drawing and VRAM **does** contain image data —
but at **x≈768**, a 768-pixel horizontal offset from where the display reads. The
compositor faithfully presents an empty region.

## 5. Regression control preserved

The black-screen binary is preserved **unmodified** and is still what `bld/`
contains:

- `integration-kit/regression-control/Syphon_Filter_2_Recompiled.exe`
- SHA-256 `97282fb89d1a0420572baad527dc16f71539fee9a69d323ef2415ab197901bff`
- identical hash in `bld/` — no rebuild has overwritten it

The two repairs above are in the **merge tree only**. Nothing has been rebuilt from
them yet, so the control and the repaired source are cleanly separable.
