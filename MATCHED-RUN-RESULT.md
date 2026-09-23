# Matched run result: fresh generation is also black (2026-09-22)

## Outcome

**The fresh-generation build is also pure black.** Regeneration alone did not resolve
it, per your corrected rule — which neither excludes a generation problem nor proves a
framebuffer defect.

| | Control | Fresh candidate (instrumented) |
| --- | --- | --- |
| exe SHA-256 | `97282fb8…` (47,596,984 B) | `eb9bb3d0…` (instrumented), `df760757…` (plain) |
| generated BIOS dispatch | 1,660,756 B | 7,384,719 B |
| generated game files | 113 (old emitters) | 99 (merged emitters) |
| `screenshot_file` | 320x240 | **384x240** |
| distinct colours | 1 | **1** |
| non-black samples | 0 / 4800 | **0 / 5760** |
| display config | x=0, y=240, 320x240 | **identical** |
| `gp0_writes` | 119,083 | 119,357 |
| VRAM at display band | empty; data at x=768 | **empty; data at x=768** |

Both boot identically:

```
bios_backend=LLE (recompiled BIOS)  bios_boot=HLE (shell skipped)  image=SCPH-1001
executing from PC=0xBFC00000
```

Both stay alive and busy with no further output.

## Guest execution differs — the fresh build progresses further

| | Control (earlier sample) | Fresh candidate |
| --- | --- | --- |
| `pc` | — | **0x8014283C** |
| `ra` | `0x80107194` / `0x80142854` | **0x80142854** |
| `g8` | `0x1F801124` / `0x807FFDC4` | **0x807FFDC4** (RAM stack) |
| `g26` | `0xBFC0193C` | `0xBFC0193C` |
| frame | 3297 → 3474 | 2885 → 3189 |

The fresh build is executing in `0x8014xxxx` **RAM** with a RAM stack pointer, whereas
the control was observed touching BIOS ROM addresses and hardware registers. So
regeneration changes guest progress — recorded as a difference, not yet as progress
toward a rendered frame.

## Your question answered: x=768 is NOT a framebuffer

Full VRAM occupancy scan (64x16 sample blocks, `vram_peek`):

```
  x:     0    128   256   384   512   640   768   896
y=0    ......#.
y=128  ......##
y=256  ......#.
y=384  ......#.
```

**VRAM is empty everywhere except a narrow column at x≈768** (plus a small patch at
896/y=128).

Measured extents of that column:

- **horizontal:** content spans roughly x=768–816 (about 48–64 pixels), then stops;
  separate patch at x=960
- **vertical:** spans y=0 through y=480 — the **full VRAM height**

A 320x240 framebuffer at x=768 would span x=768–1088 for 240 rows. What exists is a
**full-height, ~50-pixel-wide column** — consistent with texture or sprite atlas data,
not a display buffer.

**Conclusion: there is no framebuffer anywhere in VRAM.** The x=768 column is not a
misplaced display source; it is texture-like data. My earlier framing — "the frame is
rendered at x=768 while the display reads x=0" — is **withdrawn**. The display reads an
empty region because **no frame is rendered at all**.

## What this establishes

| Statement | Status |
| --- | --- |
| Regeneration resolves the black screen | **No** |
| Generated code compiles and links against the merged runtime | **Yes** |
| The two builds share the same observable symptom | **Yes** — black, same display config, same draw volume |
| x=768 contains a framebuffer | **No** — it is a narrow full-height column, texture-like |
| A framebuffer exists elsewhere in VRAM | **No** — VRAM is otherwise empty |
| The fault is generation-related | **Not established** — regeneration did not change the symptom |
| The fault is in drawing/render submission | **Leading** — draw commands arrive (`gp0_writes` 119k, `gp0_draw` 331, `gp0_fill` 313) but nothing lands in VRAM |

The last row is the sharpest new fact: the guest issues ~119k GP0 writes and hundreds of
draw/fill commands, the display is configured normally, and **VRAM receives no frame
content**. So the loss is between "draw commands accepted" and "pixels written to VRAM",
which is upstream of composition and upstream of the display-start question entirely.

## Caveats

- The fresh build progressed further in guest code than the control sample, so the two
  runs are **not** at identical guest state when sampled. The black-screen and
  empty-VRAM observations are shared, but the comparison is not perfectly matched in
  time.
- `PSX_DEBUG_PORT` is ignored when `PSX_DEBUG_TOOLS=OFF`; the plain `bld` build has no
  introspection. All captures above are from `bld-dbg` (`-DPSX_DEBUG_TOOLS=ON`), whose
  exe hash differs from the plain build's.
- One VRAM sweep connection dropped mid-run when the 90 s exit fired; the occupancy map
  above is from the 600 s run.

## Not yet done

- Establishing *why* accepted draw commands produce no VRAM content. Next probes:
  `PSX_GPU_DMA_MODEL`, `PSX_SOURCE_GPU_COMMAND_WINDOW`, `PSX_GPU_STATUS_MODEL`,
  `PSX_GL_PRESENT_PROBE` source-FBO sample, and `gl_present_ring` `src_valid`.
- The four feature tests (F1, F7, mouse-look, widescreen) — blocked on a visible frame.
- The reproducible recipe.
