# The four failing tests — relevance assessed before further merge changes

All four are **source-shape tests**: they read a `.c`/`.cpp` file as text and index
into named functions. They fail on `ValueError: substring not found` when the merge
changed which implementation won. That distinguishes "the merge dropped code" from
"behaviour broke" — worth knowing before touching anything.

Measured against both parents:

| Test | Needs | merged | `f4751d00` | `452cc0c` | Verdict |
| --- | --- | --- | --- | --- | --- |
| `test_host_input_focus_guards` | `hybrid_dpad_active` in `main.cpp` | **0** | 0 | **4** | **Real merge gap.** SF2-only function; my merge dropped it. |
| `test_gl_depth24_coherency` | `gr_display_depth_changed` in `gpu_render.c` | **1** | 0 | **1** | Symbol present. Fails on ordering, not absence — needs inspection. |
| `test_cdrom_seek_retarget` | `cdrom_clear_pending_dataready` | **9** | 9 | 4 | Symbol present and richer than SF2's. Likely a text-shape mismatch from taking baseline `cdrom.c`. |
| `test_overlay_dump_bounds` | compiles a harness | n/a | n/a | n/a | **Compile error**, not assertion: `redefinition of 'psx_ws_angle_widen'` etc. in `overlay_pair_dedup_harness.c`. |

## 1. `test_host_input_focus_guards` — a real merge gap

This is the significant one. The test comes from the **SF2 branch** and requires
`static bool hybrid_dpad_active` in `main.cpp`. It exists only on SF2 (4 hits) and
is **absent from my merged tree (0 hits)**. So a test registered for SF2's input
behaviour is currently asserting against code the merge removed.

This is very likely related to the black screen: `hybrid_dpad_active` is part of
SF2's input-routing path, and the conflict around input-frame finalisation was one
I resolved by hand. It also connects to the four `main.cpp` conflict hunks I saw
around `finalize_host_input_frame` / `g_low_latency_input` / `psx_selfcheck_input_locked`.

**Action: inspect what SF2's `hybrid_dpad_active` did and whether its removal
stranded any callers.** This is a code-loss defect of the same class as
`ws_nw_compensate_triangle`, which only compilation caught.

Also relevant: the test asserts `TURBO_PRESENT_EVERY` and `SDL_SCANCODE_TAB` are
**absent**. Both are absent in the merged tree, so those assertions pass. The
failure is purely the missing function.

## 2. `test_gl_depth24_coherency` — present, ordering-sensitive

`gr_display_depth_changed` exists in merged `gpu_render.c` (1 hit, matching SF2) and
the facade dispatch is intact (4 hits). So this is not dropped code. It uses
`require_in_order`, so it checks that named needles appear in a specific sequence —
the merge reordered something. Needs reading before judging.

## 3. `test_cdrom_seek_retarget` — likely a text-shape mismatch

`cdrom_clear_pending_dataready` appears **9 times** in merged `cdrom.c` — the same as
the baseline and more than SF2's 4. My resolver deliberately took the baseline's
`cdrom.c` ("whole file is `pending_arm`/source-model based"). So the function is
there and richer; the test indexes by an exact signature/ordering that the baseline
words differently.

## 4. `test_overlay_dump_bounds` — a compile error, not an assertion

Fails while *building* its harness:

```
overlay_pair_dedup_harness.c:192:10: error: redefinition of 'psx_ws_angle_widen'
overlay_pair_dedup_harness.c:193:10: error: redefinition of 'psx_ws_cull_keep_result'
overlay_pair_dedup_harness.c:196:10: error: redefinition of 'psx_ws_aspect_cone_result'
```

That file was a **conflict file** (both sides modified it). The harness now defines
`psx_ws_*` stubs that the merged tree also provides, so it double-defines them.
Mechanical fix, but it is a merge artifact — my resolver's doing.

## Ordering conclusion

Relevance before merge changes, as instructed:

1. **`test_host_input_focus_guards`** — must be fixed first. Code loss, plausibly
   tied to boot/input. Investigate whether anything calls the removed function.
2. **`test_overlay_dump_bounds`** — mechanical double-definition; low risk, fix in
   passing.
3. **`test_gl_depth24_coherency`**, **`test_cdrom_seek_retarget`** — read and judge;
   may be obsolete wording rather than regressions. Do not change merge output to
   satisfy a text-shape test without first establishing the behaviour it encodes.

None of these four is evidence about the black screen by itself. They are evidence
that the merge dropped or reworded code, which is consistent with — but not proof
of — a GPU/input regression.
