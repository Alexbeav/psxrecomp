# Regression narrowing: baseline vs integration, matched conditions (2026-09-22)

## Headline result

**The integration build is NOT stalled and its GPU path is NOT dead.** It boots,
executes the guest, initialises GL, and presents frames at a correct cadence.

This overturns the GPU-resolution hypothesis I was about to act on. Recorded here
before any merge change, as instructed.

## Test 1 — guest execution advancing past the reset vector

Headless, timed exit, so no window is involved:

```
PSX_HEADLESS=1  PSX_EXIT_AFTER_MS=15000  PSX_PC_PROBE=1  PSX_FRAME_REPORT_MS=3000
```

```
psxrecomp runtime: executing from PC=0xBFC00000
psxrecomp: thread scheduler = HLE (deterministic TCB)
psxrecomp: frames=790  elapsed_ms=3002   fps=262.8
psxrecomp: frames=1300 elapsed_ms=6004   fps=169.9
psxrecomp: frames=1849 elapsed_ms=9004   fps=183.0
psxrecomp: frames=2354 elapsed_ms=12010  fps=168.0
psxrecomp: PSX_EXIT_AFTER_MS reached, exiting
EXIT=0
```

**2354 frames, clean exit.** Guest execution advances far past `0xBFC00000`.

Caveat recorded: `PSX_HEADLESS=1` sets `g_headless = 1` (`main.cpp:13265-13269`),
which suppresses window creation and GL presentation. So this proves the emulation
core runs; it does **not** exercise the GL path.

## Test 2 — the windowed GL path (the one that actually matters)

Windowed, no headless, with the GL present probe:

```
PSX_NO_LAUNCHER=1  PSX_EXIT_AFTER_MS=12000  PSX_FRAME_REPORT_MS=3000  PSX_GL_PRESENT_PROBE=1
```

```
psxrecomp: OpenGL context created (3.3.0 NVIDIA 616.92)
psxrecomp: GL GPU pipeline ready (internal scale 4x, mask-bit stencil,
           texture window, GPU copy/upload)
psxrecomp: present cadence: wall-clock pacer (16.6834 ms/frame)
psxrecomp: frames=542 elapsed_ms=3013  fps=179.6
psxrecomp: frames=722 elapsed_ms=6019  fps=59.9
psxrecomp: frames=902 elapsed_ms=9022  fps=59.9
psxrecomp: PSX_EXIT_AFTER_MS reached, exiting
EXIT=0
```

**902 frames at a steady 59.9 fps** — the guest cadence — with the GL context live
and the pipeline ready. Exits 0.

## What this establishes

| Hypothesis | Status |
| --- | --- |
| Boot stalls at `0xBFC00000` | **Disproved** — 2354 / 902 frames of progress |
| Guest execution broken | **Disproved** |
| GPU path dead / crashes | **Disproved** — GL context, pipeline ready, frames paced |
| My `gpu.c` / `gpu_gl_renderer.c` resolutions broke rendering outright | **Not supported** — the render loop runs and presents |
| Black screen is a *presentation/composition* fault | **Now the leading hypothesis** |

## Honest limits of this evidence

- `PSX_GL_PRESENT_PROBE` output was requested but no probe-specific lines appeared
  in the captured stdout. So "frames presented" is inferred from the frame counter
  and the windowed GL reports, not from a direct present-probe confirmation.
- The frame counter increments in the runtime's own loop; I have not proven the
  frames contain non-black pixels. A correct frame count with a cleared
  framebuffer would look identical from outside.
- I still cannot see the screen. **A black screen with a running, presenting,
  correctly-paced renderer is consistent with a composition/source-texture fault**
  (e.g. the widescreen/EPSX display source, or the display-area rect), which is
  precisely the area SF2's native-wide code touches and where my GPU merges sit —
  but that is now a *hypothesis to test*, not a diagnosis.

## Matched-conditions note

A perfectly matched baseline comparison is **not possible for these probes**: the
widescreen baseline's exe (`75c763f1…`) contains `PSX_HEADLESS` but **not**
`PSX_PC_PROBE`, `PSX_EXIT_AFTER_MS`, or `PSX_FRAME_REPORT_MS` — it is a different
framework vintage. So the integration build was instrumented on its own terms, and
the baseline can only be compared on observable outcome (window content), not on
identical telemetry. This limitation is recorded rather than papered over.

## Next step (not yet taken)

To separate "renders correctly" from "renders black", the useful probe is whether
the composited source has non-zero content — i.e. inspect the GPU source/display
rect rather than the frame counter. Candidates already present in the binary:
`PSX_GPU_DMA_MODEL`, `PSX_SOURCE_GPU_COMMAND_WINDOW`, `PSX_DISPLAY_RING`.

No merge changes were made on the basis of the GPU hypothesis. Doing so now would
have been acting on a disproved theory.
