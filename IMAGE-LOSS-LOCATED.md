# Where the image is lost — captured evidence (2026-09-22)

## Result: the frame is drawn at VRAM x=768; the display area reads from x=0

Measured on the instrumented build (`bld-dbg`, `PSX_DEBUG_TOOLS=ON`) via the runtime's
own debug server on `127.0.0.1:4370`, all on one live run.

## 1. Displayed pixels — the decisive check

Captured with `screenshot_file` (the runtime's own canonical PNG of the PSX display):

```
{"ok":true,"path":".../black-check.png","width":320,"height":240}
```

Pixel analysis of that PNG:

```
distinct colours sampled : 1
non-black samples        : 0
top colours              : rgb(0,0,0) x4800
```

**The displayed output is pure black**, sampled across the whole image — not inferred.

## 2. The present ring agrees, and locates the failure

`gl_present_ring` for every recent frame:

```
[640,1679,"cpu",23106,[0,0,320,240],[0,0,1824,1368],[0,0,0],0,[0,0,0,0]]
   frame  path  t_ms  display_rect   letterbox        px     src
```

- presentation path is **`cpu`**
- backbuffer sample `[0,0,0]` — black
- FBO source sample `[0,0,0,0]` — also empty

So the black is **not** a blit fault: the source is empty too.

## 3. Display configuration

`gpu_state`:

```
display_x        : 0
display_y        : 240
width x height   : 320 x 240
h_display        : 600,3160
v_display        : 16,256
draw_area        : 0,0,319,239
draw_offset      : 160,120
depth            : 15
gp0_writes       : 119083     (climbed 109701 -> 119083 across ~9000 writes)
gp0_draw: 329  gp0_fill: 299  gp0_copy: 723
```

The display window is configured normally. The guest is issuing draw traffic.

## 4. VRAM sweep — content exists, but not where the display reads

```
=== horizontal sweep at y=240 (the display row band) ===
  x=0     ----
  x=128   ----
  x=256   ----
  x=384   ----
  x=512   ----
  x=640   ----
  x=768   DATA
  x=896   ----
  x=1024  ----
```

Vertically at x=768, content spans the **whole** range (y=0 through 448), including
y=240 where the display reads:

```
  x=768 y=0 nonzero=256   y=64 nonzero=224   y=128 nonzero=256
  x=768 y=192 nonzero=126 y=240 nonzero=256  y=256 nonzero=256
  x=768 y=320 nonzero=253 y=384 nonzero=256  y=448 nonzero=224
```

Sampled values are real image data, not sentinels:

```
x=768 y=272 : ffffffffdffd5b8c5b5b6f6f22313122
x=800 y=256 : bed1aaaa8787968769786b69786b3a6b
x=768 y=320 : 71786b71575c4c3c3c49493c4535383c
```

## Conclusion (located, not inferred)

| Stage | State |
| --- | --- |
| Guest issuing GP0 traffic | **yes** — gp0_writes climbing |
| Pixels landing in VRAM | **yes** — at x≈768, full vertical extent |
| Display area selection | **x=0, y=240, 320x240** |
| Region the display reads | **empty** |
| Composed/blitted output | faithfully black (correct behaviour for an empty source) |

**The frame is rendered at VRAM x=768 while the display reads x=0.** The compositor is
not at fault; it presents an empty region correctly. The fault is a horizontal
framebuffer-source mismatch of 768 pixels between where the guest draws and where the
display reads.

This is a *located* defect with a concrete hypothesis (GP1 display-start / draw-offset
handling, or the framebuffer base the guest sets vs the one the runtime honours). It is
not yet a diagnosis — the guest may legitimately use a 768-offset buffer and expect the
display to follow, which would point at display-start tracking rather than at drawing.

## Guest progression (answering the earlier question properly)

`get_registers` sampled 2s apart:

```
frame 3297  ra=0x80107194  g8=0x1F801124   (interrupt-status register)
frame 3474  ra=0x80142854  g8=0x807FFDC4   (RAM stack)
```

The guest moved from BIOS ROM (`0xBFC0xxxx`) into `0x8010xxxx`/`0x8014xxxx` RAM and is
reading hardware registers. So it is past BIOS shell and executing game-side RAM code —
**but** VRAM is empty at these frames, consistent with early init before the first
meaningful present. BIOS completion and game entry are therefore **partially**
established (RAM code reached), not fully (no visible frame yet produced at the display
area).

## Correction to prior reporting

The earlier "2354 frames means guest execution advances" was insufficient: a frame
counter increments regardless of what is drawn or displayed. This capture replaces it
with actual pixel, VRAM and register evidence.
