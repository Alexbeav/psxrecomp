# Frame Rate Unlock (framework-wide)

Goal: let any PSX title be shown at any frame rate the player picks
(30 / 60 / 90 / 120 / 144 / 240 / 360 / unlimited) **without changing how fast
the game runs**. Testbed: Alien Resurrection (PAL, SLES-02913). Nothing here
is specific to that game.

This is enhancement-tier work (CLAUDE.md Rule -1 scope note): the guest's
VBlank cadence, timers, audio and game logic stay stock. Only presentation
changes.

## Why presentation, not the guest clock

Most PSX games count VBlanks to advance their world. On PAL that is 50 per
second, so game logic can update at most 50 times a second. Raising the
VBlank rate (`psx_mod_set_native_vblank_rate`) makes such a game run faster,
not smoother. So above the game's own rate, frames have to be made on the
host side, between the game's real frames.

There are three separate rates, and they have to be kept apart:

| Rate | What it is | Who controls it |
|---|---|---|
| Guest VBlank rate | 50 Hz PAL / 59.94 Hz NTSC. Drives game logic. | Stock, never changed here |
| Game frame rate | How often the game finishes a new image (flips its display buffer). 50, 25, 17, or variable. | The game; a CPU-time budget question |
| Presentation rate | How often the window is updated. | The player's limit |

## Layers

### L0: Measure (done)

`frame_cadence.{h,c}` watches GP1(05h) display-start flips and draw activity
per VBlank and reports, for any title:

- `game_fps`: new images per second of **guest** time (independent of host
  speed);
- a histogram of how many VBlanks each image was shown for (1 = full rate,
  2 = half rate, 3+ = the game is falling behind);
- the last 256 durations.

Read it over TCP with `{"cmd":"frame_cadence","count":64}` (`reset=1` clears
the statistics), or turn on `PSX_FPS_TELEMETRY=1` for a `Game N fps` readout
in the title bar and OSD. Titles that never move the display start fall back
to "drew something this VBlank" after 8 static VBlanks.

**AR measuring pass (needs the game, so on Alex's machine):** play the same
three spots (a light corridor, a heavy room, a fight) and record
`frame_cadence` for ~30 s each. The histogram says whether AR runs at 50, at
25, or varies, and that answers the "more juice" question:

- mostly 1-VBlank frames with dips to 2: the game is CPU-bound in places and a
  guest CPU overclock would hold it at 50;
- locked at 2: the game caps itself at 25 on purpose; an overclock will not
  help, and only presentation can smooth it.

Note: this repository has **no guest CPU overclock knob** yet
(`cpu_overclock` exists on another branch, see
`docs/UPSTREAM_2026-08-20.md`). If the measurement shows AR is CPU-bound,
porting that knob (a Q16 scale on CPU-charged cycles only, devices
untouched) is the next framework piece.

### L1: Decoupled presenter + FPS limiter (done)

The OpenGL presenter already ran its own present loop inside each guest
interval. It now:

- accepts any limit from 24 to 1000 presents/s, **including below the guest
  rate** (30 on a 50 Hz game shows 3 of every 5 frames, the game is not
  slowed), plus "match display" and "unlimited";
- in unlimited mode, measures its own present cost and never starts a present
  that would run past the guest interval, so a slow GPU lowers the present
  rate instead of slowing the game;
- follows **game frames, not VBlanks**: history only advances when the game
  actually flipped, and each blend is spread over the measured duration of
  the newest frame. A 25 fps title is blended across both VBlanks instead of
  showing blend, hold, blend, hold;
- takes its guest rate from the live video standard. Previously it took
  59.94/60 Hz even for PAL titles, so turning interpolation on ran a PAL game
  1.2x fast.

Player-facing: the built-in **Frame Rate Unlock** mod
(`mods/builtin/packages/psx.presentation.frame-rate`, plugin
`runtime/src/mod_builtin_frame_rate.c`) with two options:

- *Frame rate limit*: Match display / 30 / 60 / 90 / 120 / 144 / 240 / 360 /
  Unlimited;
- *In-between frames*: Motion-adaptive blend / Crossfade / Off (limiter only:
  the newest frame, no blending, no added latency).

Developer API: `psx_mod_set_frame_interpolation(fps)` with
`PSX_MOD_FRAME_RATE_MIN..MAX` and `PSX_MOD_FRAME_RATE_UNLIMITED`;
`psx_mod_set_frame_interpolation_blend(PSX_MOD_FRAME_INTERPOLATION_NONE)` for
the pure limiter. Env for testing: `PSX_FRAME_INTERPOLATION_FPS=-1|0|24..1000`.

Verified so far: unit tests (`frame_cadence_test`,
`frame_interpolation_schedule_test`) and an OpenBIOS smoke run under a
software GL driver: the 30 cap gave 30.0 presents/s with the guest at full
speed, and unlimited kept the guest at full speed after the cost guard.
**Not yet verified on a PAL title or a real GPU.**

### L2: Motion-correct in-between frames (next)

Blending two images gives double images on anything that moves fast. The
real fix is to redraw the scene at in-between positions. Plan:

1. **Record a draw list per game frame** in the GL renderer: every primitive
   with its full state (vertices at hi-res/PGXP precision, UVs, CLUT, texpage,
   colors, blend mode, draw area, offset), plus fills/copies/uploads.
2. **Match primitives between the two newest frames** by a key built from
   things that do not move: command, texpage, CLUT, UVs (and flat color),
   with an occurrence index for repeats. Ordering-table order changes as
   depth changes, so the match is by key, not by position in the list.
3. **Re-render frame N into an off-screen target** with each matched
   primitive's vertices lerped from frame N-1 by the blend position;
   unmatched primitives draw at their frame-N position. Present that.
4. **Fall back per frame** to the L1 blend when the frame is not replayable:
   uploads or copies into the shown buffer (FMV, pre-rendered backgrounds),
   render-to-texture feedback, a camera cut (most matched vertices jump more
   than a threshold).

Same one-frame latency as today's blend. The draw list also gives the
depth-rebuilding layer (needed by fog, GI and MGS shadows) its per-primitive
Z, so L2 and the depth layer should share the recorder.

### L3: Game-side unlock (per title, enhancement phase)

For titles whose logic scales by elapsed VBlanks, a per-title mod could raise
the game's own update rate. That needs game knowledge and is a per-game
shim, so it stays out of the framework until L0-L2 are proven.
