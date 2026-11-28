# Scene capture

Scene capture is the first layer of the mod framework. It records each frame
as a typed scene that later enhancements can read: frame interpolation,
widescreen, fog, water, path tracing and a settings menu. It only observes. It
never changes what the game computes, draws, times or saves.

Code: `runtime/include/scene_capture.h`, `runtime/src/scene_capture.cpp`,
`runtime/src/scene_capture_adapter.cpp`. Tests: `runtime/tests/test_scene_capture.cpp`
(`scene_capture_test`).

## What it records

| Record | Source | Fields |
|---|---|---|
| Transform | GTE state at each RTPS/RTPT vertex | rotation RT, translation TR, H, OFX/OFY, light L, light colour LC, BK, FC, DQA/DQB, caller `ra`, rotation and translation source addresses, object key |
| Vertex | each GTE perspective projection | model-space V, view-space (MAC1..3 >> 12), exact 16.16 screen position, packed SXY, SZ, transform id, VBlank, ambiguity flag |
| Primitive | each GP0 draw command | packet address, ordering-table (OT) walk number in the frame and rank (empty OT nodes before it), opcode, kind (polygon, line, rect), textured, semi-transparent, raw texture, Gouraud, texture page, CLUT, screen x/y, linked vertex per corner, object key |

A frame closes at the first VBlank after it drew something. The last closed
frame is kept for reading.

### Linking 2D to 3D

The GPU only sees 16-bit screen coordinates. The capture links a primitive
corner to the vertex whose packed SXY matches it exactly and was projected
within `link_window_vblanks` (default 4). The GTE writes the same SXY the game
then copies into the packet, so an exact match is the normal case. Results:

- `polys_linked`: every corner found a vertex.
- `polys_partial`: some corners did.
- `polys_unlinked`: none did (2D art, HUD, sprites built on the CPU).
- `stale_links`: a match existed but was older than the window.
- `ambiguous_links`: two vertices with different view positions projected to
  the same SXY inside the window. The link takes the newest; the vertex is
  flagged.

### Objects and the camera

The PS1 GTE holds one combined matrix (camera times model) per draw batch, so
there is no separate camera to read. The capture keeps each batch's matrices
as a transform and gives it an object key: a hash of the rotation and
translation source addresses when an adapter hooks `SetRotMatrix` and
`SetTransMatrix`, or else a hash of the caller address and its order in the
frame. Separating camera from model is per-game adapter work (for example,
reading the game's own camera struct), and is not done here.

## Turning it on

- Mod: `psx.framework.scene-capture` (builtin, off by default, every game).
- Environment: `PSX_SCENE_CAPTURE=1` at start.
- Environment: `PSX_SCENE_CAPTURE_FRAMES=<on>,<off>` turns capture on at guest
  frame `<on>` and off at `<off>`, counted from boot. The last closed frame
  stays readable afterwards. Use this with a recorded input route, so the same
  frame is captured on every run.
- Debug server: `{"cmd":"scene_capture","on":1}`.

Cost when off: one global flag test at each hook.

A file replay (`PSX_INPUT_ROUTE_FILE`) refuses debug commands that could edit
or reschedule the run. It accepts the read-only ones: `scene_dump`,
`screenshot`, `scene_capture` without `on`, `window` or `adapter`, and `read_ram` inside main RAM (the first 2 MB).

## Per-game adapter

An adapter is a small TOML file named `scene_adapter.toml` beside `game.toml`.
The runtime loads it at start if it exists. Examples: `docs/scene_adapters/`.

```toml
format = 1
game = "SLUS-00594"
link_window_vblanks = 4

[hooks]
draw_ot = [0x800928EC, 0x80092A1C]   # DrawOTag, DrawOTagEnv
set_rot_matrix = [0x80095D5C]        # SetRotMatrix
set_trans_matrix = [0x80095D8C]      # SetTransMatrix
```

The addresses come from `tools/psyq_sigs` (see its README), which names the
Sony PsyQ SDK functions in a game's EXE. Each hooked address must also be in
`game.toml` `[recompiler] mod_function_entry_funcs`, because the hook is
emitted by the recompiler. Write the addresses as KSEG0 (`0x80xxxxxx`): `tools/psyq_sigs` prints KUSEG (`0x00012C84`), which becomes `0x80012C84`. Without hooks the capture still works from the GTE,
DMA channel 2 and GP0; hooks add draw-call counts and stable object keys.

A load error does not stop the game; it is reported in the `adapter_error`
field of `scene_capture`.

## Debug server commands

`{"cmd":"scene_capture"}` returns the counters of the last closed frame. Optional
fields: `on` (0 or 1), `window` (VBlanks, 1 to 60), `adapter` (a path to load).

Measured on Metal Gear Solid (SLUS-00594), Dock gameplay, guest frame 18700:

```json
{"ok":true,"on":0,"window":4,"adapter_game":"SLUS-00594","adapter_error":"",
 "frames":60,"frame":60,"prims":495,"polys":420,"lines":62,"rects":13,
 "polys_linked":385,"polys_partial":33,"polys_unlinked":2,"vertices":639,
 "transforms":60,"objects":1,"ot_walks":1,"draw_calls":1,"stale_links":7,
 "ambiguous_links":812}
```

`frames` and `frame` count closed frames since capture was turned on, not
guest frames.

`{"cmd":"scene_dump","what":"prims","offset":0,"count":500}` pages through the
last frame. `what` is `prims`, `vertices` or `transforms`; `count` is at most
4000.

## Measured results

One gameplay frame per testbed, captured headless from a recorded input route,
with capture on for the 120 frames before it.

| Game | Guest frame | Polygons | Linked | Partial | Unlinked | Linked share | Vertices | Transforms | Draw calls |
|---|---|---|---|---|---|---|---|---|---|
| Metal Gear Solid (USA, SLUS-00594) | 18700 | 420 | 385 | 33 | 2 | 91.7% | 639 | 60 | 1 |
| Alien Resurrection (Europe, SLES-02913) | 4200 | 419 | 237 | 119 | 63 | 56.6% | 480 | 37 | 0 |

Alien Resurrection records no draw calls although its frame has two OT walks.
Its `DrawOTag` hook is in the main EXE; the gameplay frame does not reach it.
The likely cause, not yet confirmed, is that gameplay code runs from overlays
with their own copy of the library: `tools/psyq_sigs` finds most of the game's
code and its GTE calls outside the main EXE. Its lower linked share and 30 ambiguous
links are not yet explained either.

## Limits

- Lines link their first segment only.
- The source-model linked-list DMA path (`gpu_ll_source_model`) reports no
  walk start or end, so its primitives carry `ot` -1 and count no walks.
- A vertex built on the CPU (not projected by the GTE) never links.
- Capture follows the main GTE path; speculative and replay GTE runs are
  ignored so they cannot add false vertices.
