# SF2 modernization pass 2 — widescreen and direct mouse

Updated: 2026-08-04

Status: accepted by human A/B testing for Mission 1. The enhanced build now
provides correct-aspect native widescreen and direct mouse control without the
previous edge culling, black-margin flashes, or rhythmic triangle corruption.
The frozen 4:3 compatibility build remains separate.

## Scope

This pass adds an opt-in enhanced build without changing the frozen 4:3
compatibility executable. Its acceptance surface is production-quality 16:9
under software and OpenGL plus direct relative-mouse yaw/pitch for third-person
camera control and first-person aiming. Retail owns gameplay and controller
input; scripted camera ownership suppresses the enhancement.

PGXP and high-refresh work are the next pass. They remain disabled in this
checkpoint so this accepted build is a stable differential oracle.

## Consult-test-return classification

| Lead | Source | Initial classification | Required local check |
|---|---|---|---|
| Persistent draw/display pages are authored state | PSX-Ports GPU contract and hybrid handoff | confirmed by compatibility work | retain canonical VRAM/page hashes in every wide comparison |
| World projection and fullscreen-effect expansion are distinct policies | hybrid handoff | confirmed behavioral contract | classify world, HUD, FMV, fade, scope and matte submissions independently |
| Relative mouse must bypass retail controller acceleration | hybrid runtime | narrowed to a reusable semantic design | prove the SCUS-94451 execution boundaries and affected retail angle state |
| Scripted camera owner suppresses chase pitch | hybrid runtime | confirmed behavioral contract | prove owner transitions and zero writes while the player does not own the camera |
| `0x80053464` is the chase/facing boundary | hybrid runtime | confirmed for the direct-vector bridge | exact word/AOT leader, 32 bounded live state-0 hits, and player camera ownership proved locally |
| `0x800539D0` is the manual-aim boundary | hybrid runtime | narrowed, no hook required | exact word/AOT leader and 16 held-L1 executions proved locally; the shared vector boundary applies direct aim deltas without duplicating retail code |
| Native-wide should use PSXRecomp's separate compositor surface | framework documentation | confirmed | software and OpenGL routes classify 4:3 surfaces separately and present gameplay from a 512x240 guest-wide surface |
| The generic 320/224-or-241 cull signature applies to SF2 | framework detector | contradicted | zero emitted calls; adding SF2's observed 240-line vertical signature also emits zero calls, so no cull hook is claimed |
| PGXP and high refresh can be copied as completed SF2 features | initial assumption | contradicted | hybrid handoff records both as remaining work; defer them from this pass |
| Outdoor sky/SCRIM needs ownership distinct from arbitrary far GTE geometry | hybrid SCRIM description plus bounded GP0/OT evidence | narrowed; owner still open | use topology and live invariance checks rather than OT rank or payload identity |
| A far-depth or exact GTE caller owns the Mission 1 sky | local dome probes | contradicted | all bounded candidates either leave the gap or deform world/actors; ship no GTE site |
| The first textured OT rank owns the finite outdoor backdrop | local packet census and live A/B | contradicted | human parachute-scene evidence proves the rank contains projected curved environment geometry; aspect stretching corrupts both reveal margins |
| A horizontally complete auxiliary quad owns the complete output width even when it is not full-height | human cinematic-matte evidence plus hybrid fullscreen-effect contract | confirmed under hidden software/OpenGL; human pending | visually verify Mission 1 top/bottom mattes and later scope/NVG/fade transitions |
| Software high-resolution polygon seams are caused by backdrop stretching | software live A/B | contradicted | seams are pixel-identical with ownership on/off; retain as a separate renderer-precision issue |

No sibling repository is modified. External addresses and structure layouts do
not enter committed configuration until their local checks pass.

## Acceptance gates

1. The baseline launcher still runs the frozen 4:3 executable and separate
   cards.
2. Mouse buttons may use PAD actions, but mouse motion never emits D-pad or
   analog-stick input.
3. Direct yaw and pitch are non-inverted by default, separately configurable
   for chase and manual aim, focus-safe, bounded and deterministic.
4. Scripted cameras retain ownership; player mouse control resumes without a
   handoff snap.
5. 16:9 expands retail world projection/culling rather than cropping or merely
   stretching a 4:3 scene.
6. FMVs remain authored 4:3. HUD, pause/map, fades, scopes, NVG and cinematic
   mattes follow typed presentation policies.
7. Software and OpenGL 4x produce deterministic framebuffer/display evidence;
   final-present evidence proves the window aspect.
8. The authentic Mission 1 route passes automatically, then the enhanced build
   is handed to the user for the Missions 1--8 Disc 1 run.

## Evidence log

- The locally configured input is the exact supported executable: SHA-256
  `75A360BF7465DFDEC85C14F9BA93862AAE2531B48D83FD8D82BA8C9FFFA13D33`.
- `0x80053464` contains retail word `0x8EA30034` and is an AOT block leader
  inside function `0x800523F4`.
- `0x800539D0` contains retail word `0x8FA20010` and is an AOT block leader
  inside function `0x80053664`.
- Those facts prove identity and native ownership only. They do not yet prove
  the semantic role of either boundary by themselves.
- A clean software/headless process followed the authentic publisher, Eidetic,
  legal, ZINTRO, TITLE, retail menu, state-8 and state-0 route. A bounded watch
  armed before boot recorded 32 executions of `0x80053464`, beginning only
  after Mission 1 player control. The same route proved application state 0,
  live player `0x801A0ECC`, camera wrapper `0x801FD308`, owner
  `0x801A0ECC`, and authoritative player movement. Evidence is ignored under
  `lab/sf2/local/pass2-mouse-proof-clean/`.
- The direct bridge is generated at the verified block leader. Generation
  fails if the resident word differs from `0x8EA30034`; runtime application
  additionally requires state 0, valid RAM pointers and camera owner == player.
  Mouse motion is never translated to PAD directions when this bridge is
  enabled. Mouse buttons remain ordinary retail PAD actions and physical
  controllers retain their existing path.
- A focused title-neutral unit regression covers chase yaw/pitch, manual-aim
  yaw/pitch, non-inverted Y, scripted-owner rejection, stale-motion discard and
  executable-word rejection. The direct GCC build passes.
- Held retail L1 produced 16 bounded executions of `0x800539D0`. This confirms
  the reported aim path is live, but the implemented bridge does not need a
  second hook: at the shared verified vector boundary, ordinary retail PAD
  state selects chase versus aim scaling. A clean OpenGL route applied chase
  delta `(40,20)` as `(yaw=30,pitch=20)` and held-L1 aim delta `(-24,-12)` as
  `(yaw=-24,pitch=-12)`; both counters advanced once and controller input
  remained on the SIO-visible PAD path.
- The initial one-vblank motion lifetime was contradicted by the live nested
  callback cadence: ownership was valid but pending deltas expired before the
  semantic block. A bounded four-callback lifetime passes the focused unit and
  both live modes, while scripted-owner rejection still discards stale input.
- `gte_game_mode` is enabled from local evidence: Mission 1 gameplay produces
  sustained GTE projection (2,497 vertices at a representative checkpoint),
  while TITLE, the depth-24 movie, and state-8 briefing produce none. This
  keeps authored 2D/full-frame screens 4:3 without game-state forcing.
- The clean hidden software route at
  `lab/sf2/local/pass2-hidden-software-b/route.json` passes authentic startup,
  TITLE, New Game/One Player, aircraft FMV, state 8, state-0 ownership and
  authoritative movement. Its present ring reports `native43` at TITLE, movie
  and briefing, then `wide` at player ownership with `nw_extra=128` for the
  live 384-pixel display.
- The clean hidden OpenGL route at
  `lab/sf2/local/pass2-hidden-opengl-a/route.json` passes the same gates plus
  the automated semantic-mouse assertions. TITLE and state 8 use 4:3 VRAM
  presentation, the 24-bit movie uses the coherent CPU scanout path, and all
  three target a centered 1440x1080 rectangle. Gameplay uses the wide FBO from
  guest `[0,0,512,240]` to the full 1920x1080 destination. The corrected debug
  fields report `configured=1,active=1,mode=2` there.
- Automatic cull discovery emits zero retail calls with the framework defaults
  and still emits zero after adding the locally observed 240-line vertical
  signature. No address or width was guessed. Campaign edge visibility remains
  a human acceptance item, starting with Missions 1--8.
- Framework validation passes 49/49, the focused mouse unit passes, generated
  retail C remains ignored/unmodified, and the complete footprint is 7.721 GiB.
- The first origin-naive full-width candidate reached authentic state 0 with
  native-wide active but counted 65,148 checks and zero expansions. The exact
  divergence is that SF2 authors gameplay screen edges at `-192..+192` before
  GP0 draw offset `+192`; comparing raw packet X with framebuffer `0..384` can
  never match. The title-neutral helper now receives the authored origin
  derived from `draw_area_left - draw_offset_x`, and a recognized effect cannot
  subsequently be HUD-shifted. Centered-origin rectangle/quad regressions,
  strict C99 compilation, Release link and 49/49 tests pass. Candidate SHA-256
  is `6D618BC788D33E40ED57EC42D97658CC4BC6A5B28AB0EA30779B8FC7BF2F2568`;
  clean hidden-window software and OpenGL routes pass TITLE, aircraft FMV,
  state 8, state-0 ownership and movement; both advance to exactly 7,035
  expansions at player ownership. Human visual acceptance remains open.
- Mission 1 packet census initially correlated the finite reveal with the first
  OT rank that submits textured polygons, but the bounded GP0 topology and
  human parachute evidence contradict ownership: the rank contains connected
  projected environment geometry. The production profile disables the rank
  transform; semantic background/SCRIM ownership remains open.
- Global far-depth projection and every bounded exact GTE caller were rejected:
  they either left the reveal wedge or deformed terrain, actors and foreground.
  Raw palette/source gates isolated the layer but remain diagnostic evidence,
  not production ownership.
- Earlier bounded OpenGL/software A/B showed the rank transform changed the
  finite reveal, but that correlation was insufficient: later human evidence
  proved it also corrupts world geometry in the margins. Software's broader
  polygon-colour seams remain a separate open precision issue.
- The exact final executable passed a blank-card OpenGL control route entirely
  through resident AOT plus interpreter fallback. Reusing older broad overlay
  captures then lost the endpoint at the state-8 exit and was rejected. Shards
  rebuilt only from the successful final-executable capture pass the same clean
  route with 15,831,907 resident-AOT, 6,862,443 compiled-overlay and 542,932
  interpreter dispatches (7.3316% of the overlay tier), 25 image loads, zero
  lost CD INT1 events, 1,210 SPU key-ons and matching final player XYZ.
- The focused backdrop-owner unit remains a model test only and is not evidence
  for the contradicted production rule. The origin-aware fullscreen-effect unit
  and registered 49/49 suite pass. Generated retail C/captures/cache remain
  ignored and unmodified by hand.

## Human Disc 1 acceptance focus

Use `tools/start_sf2_disc1_validation.ps1` for the recorded enhanced run and
`tools/start_sf2_baseline.ps1` for a fresh 4:3 compatibility run. The separate
`tools/start_sf2_baseline_ab.ps1` clones a completed enhanced session's retail
cards into new baseline-only writable state for a later-mission A/B without
modifying the evidence. Full instructions are in `DISC1_VALIDATION.md`.

The bounded monitor drains the software/OpenGL present rings, records
configuration hashes, application/widescreen transitions, low-rate
player/camera samples and the exact final SIO PAD timeline. A new generic GPU
counter distinguishes fullscreen rectangles merely checked during native-wide
gameplay from those actually expanded across the reveal margins. It records no
pixels, RAM/audio dumps or overlay payloads and never writes guest state. Its
unit regression, PowerShell parsing, 49/49 framework suite and an invisible
483-frame lifecycle/finalization smoke test pass.

During the Missions 1--8 run, record the first reproducible issue in these
categories:

- actors or world geometry disappearing only near a 16:9 edge;
- HUD, pause/map, scope/NVG, fade or cinematic matte stretching or failing to
  cover the full output;
- scripted-camera motion being disturbed by mouse input or snapping on player
  handoff; and
- chase versus first-person sensitivity, sign, or axis behavior.

## Accepted checkpoint

Human testing on 2026-08-04 accepted the dense-world-submission candidate:

- character proportions match the 4:3 control;
- the native-wide margins expose real scene content;
- the earlier actor/sky edge culling is absent;
- rhythmic triangle and black-edge flicker are absent; and
- direct mouse control remains usable.

The owning distinction is the linked-list DMA submission, not a reused packed
SXY value, ordering-table rank, draw address, or presentation rectangle. SF2
submits three setup-only lists, one dense world polygon list, and small
auxiliary/UI lists during the representative Mission 1 scene. The title profile
classifies a world submission with a bounded polygon-count predicate; the
generic runtime owns only the traversal and inverse projection operation. A
focused title-neutral regression covers the predicate, 4:3 identity, native-
wide scale/inverse composition, and pre-transform PS1 primitive rejection.

Source checkpoint: `a2b951c`. Rebuilt Release executable SHA-256:
`1A6B0DE5FDB4CCC7DE2D6AD99BAE89A878741B508114978D16465C12DDF1C529`.
The executable and generated retail code remain ignored local artifacts.

PGXP and high-refresh presentation remain intentionally outside this pass.
Their bounded plan is recorded in `NEXT_ENHANCEMENTS_2026-08-04.md`.
