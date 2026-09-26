# Render passes: true in-between frames

Frame blending ([FRAME_RATE.md](FRAME_RATE.md)) crossfades finished frames; it
cannot show objects in positions the game never drew. A **render pass** lets a
trusted game plugin redraw its scene between two logic ticks with the game's
own draw code, with objects and camera placed part of the way from one tick
to the next. The pass never changes the game: guest time is frozen while it
runs and the machine is restored bit-for-bit afterwards. Its only product is
an image the presenter shows between the game's own frames.

Default off. Nothing happens unless a plugin calls the API below.

## API (mod_plugins.h)

```c
uint32_t psx_mod_render_pass_plan(uint32_t period_vblanks,
                                  uint32_t shown_after_vblanks,
                                  uint32_t *alpha_q16, uint32_t max);
int psx_mod_render_pass(struct CPUState *cpu, const PSXModRenderPass *pass,
                        PSXModRenderPassFn fn, void *user);
```

Call both from an emulation-thread function-entry hook at the point where
the game has finished the logic of tick n+1 but the display still has to
flip to frame n. For a PsyQ double-buffered loop that is the entry of the
`VSync(0)` that precedes `PutDispEnv`.

1. `psx_mod_render_pass_plan(P, D, a, max)` returns the phases (Q16, in
   (0, 1)) at which the presenter will show frame n: its output deadlines
   during the P VBlanks the frame stays on screen, starting after D more
   VBlank presents. When the host cannot afford them all, an evenly spread
   subset is returned. 0 means "no passes this frame" (see Gates).
2. For each phase, `psx_mod_render_pass()` with the display rect the next
   flip shows (the DISPENV rect). `fn(cpu, user, alpha)` may write guest RAM
   and call guest functions with `psx_dispatch_call()`; it returns nonzero to
   keep the image. The first pass after a plan also captures frame n's own
   image (phase 0).

The presenter uses `PSX_MOD_FRAME_SOURCE_FLIP` to see the
flip, and a plugin that supplies passes normally selects
`PSX_MOD_FRAME_INTERPOLATION_HOLD`, so wherever no pass image applies the
newest game frame is repeated rather than crossfaded one frame late.

## What a pass may and may not do

While `fn` runs (`g_psx_render_pass_active`):

| Area | Behaviour | Where |
|---|---|---|
| Guest clock | cycles are counted (GTE and mult/div deadlines work) but no device is serviced, no VBlank or device event fires | `psx_cycles.c` freeze |
| Interrupts | never delivered | `interrupts.c` |
| GPU DMA | linked lists and delayed completions finish synchronously | `dma.c` |
| RAM / scratchpad stores | written directly, bypassing code-page tracking, overlay watch, write traces and fingerprints | `memory.c` `render_pass_store` |
| MMIO stores | allowed: GP0, GP1 DMA mode / info, GPU and OTC DMA channels, DPCR/DICR, I_STAT/I_MASK. Dropped and counted: SPU (key-ons), CD, timers, SIO, MDEC, other DMA channels, memory control | `memory.c` |
| VRAM | only the declared rect; writes that bypass the scissor elsewhere are journaled and rolled back (refused under native-wide) | `gpu_gl_renderer.c` |
| Runaway code | an 8 M guest-cycle watchdog rolls the pass back | `render_pass.c` |

After `fn` (success or not) everything is restored: CPU state with the GTE,
2 MiB RAM, scratchpad, I-cache tags, I_STAT/I_MASK, timers, DMA and GPU
registers (without the widescreen side effects of a savestate load), the
VRAM rect (hr colour, mask stencil, raw 16-bit mirror, native-wide band, CPU
VRAM rows), the renderer's coherency bookkeeping, and every clock value.
After 8 faults (watchdog or refused writes) passes stay off for the session.

## Gates

The plan returns 0 in netplay, rollback resimulation, self-check, rewind,
lockstep, an exception, while a GPU DMA list is in flight, on anything but
the OpenGL renderer with FLIP-source interpolation, while the presenter is
suspended (FMV), and when no VBlank was presented since the last plan (debug
turbo, headless).

## Presentation and budget

The presenter keeps two generations of images: the frame on screen and the
one being built for the next flip. A generation becomes current when the
FLIP source sees the display flip to its rect; from that interval's start
its phases map onto host time. At each output deadline the newest image at
or before the deadline's phase is shown, crossfaded into the next one when
passes were shed. Deadlines that fall due while passes run are presented
between passes, so the frame on screen keeps moving.

Passes cost host time inside the game's frame. The budget per frame is
`PSX_RENDER_PASS_BUDGET` percent (default 80) of the presenter's idle time
plus the presents beyond two per frame, learnt from the previous frame, over
a smoothed per-pass cost. Nothing ever slows the guest down: when the budget
runs out, fewer passes are rendered.

Pass images are kept at internal resolution (the size the presenter
captures); two generations fit a 256 MiB budget, which limits passes per
frame at very high internal resolutions.

## Verifying a title

- `render_pass_stats` (TCP): passes, shedding, faults, dropped device
  stores, timing split (backup / guest code / capture / restore), presents
  made from pass images.
- `render_pass_dump path=<dir> count=<n>`: PNGs of the next n frames' images
  (the game's own frame, then each pass in phase order).
- `PSX_RENDER_PASS_VERIFY=1`: hash CPU, RAM, scratchpad, I-cache, interrupt,
  timer, DMA and GPU state and read back the VRAM rect before and after every
  pass; `verify_mismatch` must stay 0.
- `frame_fingerprint reset_on_load=1` then a savestate load: the per-frame
  write/MMIO/cycle fingerprints of a run with passes must equal a run
  without them.

Tests: `render_pass_plan_test` and `render_pass_freeze_test` (runtime ctest),
`render_pass_guards` (source guard, recompiler ctest).
