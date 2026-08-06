# Current objective — high-refresh presentation after accepted PGXP pass 1

## 2026-08-05 user result — spatial R2 rejected and reverted

The user rejected spatial R2. Gameplay did not maintain steady speed and could
run considerably faster than regular play; widespread texture cracking exposed
seams between independently replayed polygons. These are disqualifying failures
even though the hidden routes preserved endpoint guest state and alpha=1 replay
was pixel-exact.

The result invalidates two automated assumptions. Endpoint equality did not
prove invariant guest progression per wall-clock second under visible output,
and exact reconstruction at alpha=1 did not prove watertight shared edges at
intermediate alpha. Conservative per-packet matching can select different
motion histories for adjacent triangles, so it is not an adequate topology
owner for interpolation.

The R2 shortcut is disabled. All spatial capture, matching, replay, output,
debug endpoint, provenance hooks, probe test and route census plumbing have
been removed from the active source. The accepted PGXP source checkpoint
`2eebc41`, native-wide presentation, direct mouselook and retail timing remain
the baseline. R2 evidence is retained only as rejected research.

Future 60 FPS work must reproduce the authoritative object/camera snapshot
boundary used by the SF1 approach, or establish an equally coherent SF2 owner.
It must prove wall-clock guest progression, shared-edge/topology coherence, and
HUD/FMV/fade exclusion before another user handoff. Packet-nearest-neighbor
replay and whole-frame temporal blending are both retired.

Updated: 2026-08-05

## 2026-08-05 user result — R1 rejected and retired

The user rejected the 60 Hz linear presentation candidate: motion felt the
same as the accepted build and moving imagery acquired visible ghosting. This
is decisive visual evidence even though the automated semantic and cadence
gates passed. R1 must not advance to qualification.

The result matches the implementation. R1 crossfades the previous and current
retail render targets; it increases host swaps but does not estimate where
geometry belongs between authentic frames. The existing motion-adaptive mode
only suppresses blending for pixels whose color delta exceeds a threshold. It
can trade ghost trails for stepped/shimmering changed regions, but it cannot
create correct intermediate motion, so no R2 user handoff will be made from
that blend family.

The candidate-only parser relaxation to 60 Hz was reverted to the pre-candidate
90 Hz floor, and the R1 shortcut now exits with a rejection notice rather than
launching the game. The executable, config and evidence remain in ignored local
paths for audit. The useful route improvements remain: interpolation telemetry
at semantic checkpoints and oldest-entry selection for newest-first sector
history, with a focused regression.

The next high-refresh investigation must prove a spatially correct
presentation method before another user handoff—for example renderer-side
geometry/camera reprojection with complete provenance and explicit HUD/effect
exclusion. It must not change retail simulation, VBlank, gameplay state or
authored timing. Until that exists, the accepted PGXP build remains the user
build and there is nothing further for the user to test.

Updated: 2026-08-05

## 2026-08-05 candidate handoff — 60 Hz presentation R1

The first presentation-only high-refresh candidate is green at the automated
handoff gate and is ready for user observation. Retail simulation, VBlank and
authored timing remain unchanged; the isolated config enables the existing
OpenGL presentation interpolator at 60 Hz with VSync off. The parser now
accepts the runtime-supported floor of 60 Hz (59 remains rejected), and route
checkpoints include bounded `gl_interp` state.

Two clean-card hidden OpenGL Mission 1 routes passed. The corrected repeat
compares with zero errors against the frozen native baseline: startup hash
`6bf9af29fcb41a118455be11a25893937f59e64e3d2bb173bc58f937d4e1bff2`,
input hash `c518cd5e1e597e70eebc0e82e8b305dcc56f6499f8672a52867cdc89bdefd650`,
matching fingerprints at all five checkpoints, and final player XYZ
`(-5606,2036,7529)` with health 150. Interpolation is enabled but suspended
with empty history and zero swaps at TITLE, the aircraft movie and state 8.
After retail player/camera ownership it has two history frames and presents at
exactly 60.00 Hz from approximately 19.9 authentic captures/s. The repeat ends
with 1,882 interpolated swaps and zero lost CD INT1.

Evidence is under
`evidence/high-refresh-r1-route-repeat-20260805-042525`; the initial valid route
is `evidence/high-refresh-r1-route-20260805-041448`. The latter exposed a route
artifact: `cdrom_sector_history(count=1)` returns the newest match, so its
startup frame depended on host polling. The route now requests a bounded
matching history and selects the oldest guest entry; its focused regression
passes. The earlier `041241` directory is an invalid launch because developer
endpoints were compiled out, not a retail result.

The separately named candidate executable SHA-256 is
`0199B159814B6E6FD047AC8993FEAFAB3D2C1F7990629A2F95AD63A282C9B851`.
The exact user shortcut is `lab/sf2/local/generated-disc1-r2-load-delay/Launch
SF2 High Refresh R1.bat`. Ask the user to inspect world-motion smoothness,
camera/mouse response, HUD stability, transitions/FMV, and any shimmer or
double-image artifact. Stop after handoff; do not run the full suite, commit or
push until explicit approval advances qualification.

The ignored footprint is 18.71 GiB and no owned runtime/route process remains.

Updated: 2026-08-05

## 2026-08-05 handoff — current-hash native route restored

The high-refresh prerequisite is now green. Deterministic first-live rank
bisection isolated the first failing native candidate to rank 306: entry
`0x8001DA48`, code CRC `0x58690F42`, in shard
`0001C000_7A4D33C6.dll`. Limits 305 and below pass; limit 306 fails at guest
frame 24003; an otherwise fully native exact block of only `0x8001DA48`
passes. The block was diagnostic only and is not retained.

Read-only comparison with the configured SCUS-94451 executable explains the
failure. The 8,196-byte captured image differs from resident text at two words:
an unrelated data word at `0x8001C754`, outside the compiled function, and a
resident JALR `0x01204009` changed to NOP at `0x8001DA9C`, inside the exact
guarded range `0x8001DA48..0x8001E004`. The earlier whole-capture
`PSX-OVL-005` classifier therefore reported zero matches: the unrelated data
word hid a pure control-flow-presence change in the emitted candidate.

The generic correction classifies each compiled function over its exact range
manifest instead of classifying the complete dirty capture. If any native
identity is otherwise resident text and changes only the presence of control
flow, `compile_overlays.py` atomically publishes an `unpromoted` sidecar. Cache
scans on Windows and POSIX omit marked shards, and the loader rechecks the
marker immediately before loading to close the publication race. Captures and
compiled audit artifacts remain intact; only native authority is withheld.

Two clean hidden-window OpenGL Mission 1 routes pass through retail state-0
player movement. Their comparison has zero errors, identical startup/input
hashes and checkpoint fingerprints, identical card hashes, and final player
XYZ `(-5606,2036,7529)`. The repeat records 580 registered candidates,
approximately 145.82 million compiled-overlay dispatches and 147.9 thousand
interpreter fallbacks, with zero candidate/range overflow. The complete
16,384-entry native ring contains no `0x8001DA48` call. Evidence:

- `evidence/high-refresh-resident-range-gate-20260805-034046`
- `evidence/high-refresh-resident-range-gate-repeat-20260805-035033`

The exact rebuilt diagnostic executable SHA-256 is
`59557D6D1640FFEE236504E5EBC16E5C12AFE1ADA2D1EC37A9834EDDB3E6C0BB`.
Focused classifier, marker, serializer-bounds, capture-history and crash-ring
tests pass 5/5. `git diff --check` passes. The ignored footprint is 17.43 GiB.
No runtime/route process remains. Per the development workflow, the full suite,
commit and push remain deferred until the candidate advances to qualification.

The next implementation objective is the first presentation-only high-refresh
candidate described in `HIGH_REFRESH_PASS1.md`: retail simulation and authored
timing remain at 59.94 Hz while host presentation interpolates independently.

Updated: 2026-08-05

## 2026-08-04 shutdown handoff — current-hash overlay divergence isolated

Work continues on `experiment/sf2-high-refresh-pass1` from source checkpoint
`2eebc41`. Presentation interpolation has **not** started. The prerequisite is
still to restore current-hash compiled-overlay execution without changing the
accepted retail route.

The current `cg9_82e77d3e_gcd6e97ca6` cache is valid and active. A clean hidden
OpenGL run registered 443 candidates across 12 runtime regions and reached
approximately 145.6 million compiled-overlay dispatches with approximately
97.8 thousand interpreter fallbacks, zero invalidations, zero stale blocks,
and zero candidate overflow. It nevertheless exits deterministically at guest
frame 24003 while the route is scheduling the state-8 briefing exit. The same
route passes with all overlay execution interpreted, so current-hash native
execution remains the owner class, but the final native call is not the root
cause by itself.

Three bounded controls are now recorded:

1. All-interpreter execution passes state-0 movement.
2. Blocking only `0x8000293C` still exits at frame 24003 and moves the final
   unreturned owner to sibling entry `0x80002954`.
3. Blocking all captured entries into the shared OpenBIOS RAM exception body
   `0x80002814..0x800029CC` still exits at frame 24003. It leaves no native call
   in progress, retains approximately 142.9 million native calls, and records
   approximately 9.1 million blocked/fallback opportunities.

Therefore a final-call blocklist is contradicted. Resume with bounded
`PSX_NATIVE_RANK_LIMIT` bisection to find the earliest candidate-set boundary
that changes the semantic result, then use an exact candidate control and fix
the owning generic invariant. Initial conservative bounds are `[0, 449]`; the
first clean control should use limit `224`. Do not retain an SF2 blocklist as
containment.

Generic diagnostics now expose the always-on native call ring in ordinary
atexit/crash reports. The additive capture loader unions current, directory,
and legacy-single-file evidence. The runtime also migrates a legacy
`overlay_captures.json.d` file into an immutable directory contribution while
retaining a recoverable sibling. Focused tests pass. Full details, exact local
evidence paths, rejected hypotheses, build identities, and resume commands are
in `HIGH_REFRESH_PASS1.md`.

The historical accepted PGXP executable hash remains documentation evidence,
but that ignored binary was accidentally rebuilt during diagnostic source-sync
work. It has been reconstructed from exact source checkpoint `2eebc41` and the
unchanged accepted config (whose hash still matches), but the non-reproducible
Windows link now hashes to
`DE284A5BBBF7C783CC68A90C97937CF8BB9B1AD6B780581178B83E51794C95F2`.
The frozen source branch and accepted configuration remain intact. All further
diagnostics use the separate `build-high-refresh-pass1-diag` tree.

Updated: 2026-08-04

## 2026-08-04 accepted PGXP pass-1 checkpoint

Human Mission 1 testing accepts PGXP pass 1 on top of the accepted native-wide
and direct-mouse profile: culling is absent, aspect is correct, and polygon
wobble and texture swimming are visibly reduced. The accepted OpenGL candidate
is `build-pgxp-pass1-widescreen-r2`; executable SHA-256
`CA6CE21CB71CE71C6A270939BCD12752FCFC0FF224B7761B94F93DC31FD87DC3` and
generated-config SHA-256
`80BA2A4D702131EA5288B5EB12F39848C64AFFD9370E4737BEA389F1CE08FE45`.

The title-neutral implementation preserves guest-visible GTE registers, RAM,
and canonical PS1 VRAM. It attaches exact address/generation provenance to
SWC2 stores and gives a triangle subpixel position and reciprocal-depth UVs
only when all three packet vertices match. Every incomplete primitive falls
back atomically to native integer coordinates and affine texture mapping. Both
software and OpenGL consume the same renderer-neutral metadata. At the final
OpenGL Mission 1 checkpoint, 14,823 of 390,433 submitted triangles were exact;
the selective coverage explains why this is an accepted first pass rather than
a claim of complete PGXP coverage.

Two clean hidden-window OpenGL routes pass the authentic startup and retail
Mission 1 route with identical startup/input hashes, matching normalized
fingerprints at stable TITLE, aircraft FMV, state 8, player ownership, and
movement, and identical final player XYZ `(-5606,2036,7529)`. An independent
software route passes the same semantic gate and all cross-renderer normalized
checkpoint fingerprints match. The software poll first observed `LEGAL.STR`
19 guest frames later, so exact cross-renderer movie-observation timing is not
claimed. The full registered suite passes 50/50 and the focused canonical-VRAM
regression passes.

Overlay ownership remains intentionally honest: the new codegen hash does not
match the copied older cache, so qualification measured approximately 15.99M
resident-AOT calls, zero compiled-overlay dispatch, and 13.53M interpreter
fallbacks. Rebuilding the current-hash overlay cache and profiling renderer,
resident, compiled-overlay, and fallback host time is the next prerequisite.
After that, implement 60 Hz presentation interpolation while retail simulation,
timers, CD, SPU/XA, PAD sampling, scripts, and authored timing remain unchanged.
See `PGXP_PASS1.md` and `NEXT_ENHANCEMENTS_2026-08-04.md`.

Updated: 2026-08-04

## 2026-08-04 accepted modernization checkpoint and next objective

Human A/B testing accepts the current enhanced build as visually correct and
playing well for the exercised Mission 1 scenes. The native-wide solution keeps
retail-visible GTE projection widened, then compensates raster X only within the
dense polygon-owned linked-list DMA submission. It does not classify by SF2
address or reused packed coordinate. The 64-polygon discriminator is explicit
title-profile data and zero disables it. Source checkpoint `a2b951c`; rebuilt
Release executable SHA-256
`1A6B0DE5FDB4CCC7DE2D6AD99BAE89A878741B508114978D16465C12DDF1C529`.

The compatibility and enhanced launch/build identities remain separate. The
rejected value-provenance cache has been removed from production code. CLI
Release generation and link pass; all 50 registered tests pass when the required
`PYTHONUTF8=1` environment is present, and the focused projection-composition
and primitive-rejection tests pass with warnings-as-errors.

The next objective is PGXP, followed by high-refresh presentation. PGXP must
consume complete renderer-neutral GTE provenance while leaving guest-visible
GTE/RAM values unchanged and falling back to native PS1 coordinates on every
unmatched primitive. The first 60 FPS milestone is presentation interpolation
with retail simulation unchanged. A true simulation-rate unlock is prohibited
until SF2's authoritative timestep is identified and gameplay, scripts, audio,
device clocks, and input semantics remain invariant. See
`NEXT_ENHANCEMENTS_2026-08-04.md`.

The payload-free return is published to the private PSX-Ports corpus at commit
`6fd986a`. It records the linked-list projection-owner rule as a candidate, not
a stable finding, and names SF3 recomp as its independent validator.

Updated: 2026-08-03

## 2026-08-04 per-triangle projection-provenance candidate

Live A/B evidence contradicts both the OpenGL wide-mirror surface and a single
display page as the rhythmic triangle-flicker owner. The broad ordering-table
inverse is replaced by a complete three-vertex GTE value-provenance rule. CPU-
authored and partially matched triangles now fail closed; each hardware half of
a quad is classified independently. Exact pre-squash 16.16 X is retained in the
generic GTE geometry cache, so the host no longer applies an approximate second
FOV transform.

Focused projection, GTE provenance, primitive-reject, and recompiler parser
tests pass. Release build `build-modern-pass2-provenance` completes. Two clean
hidden OpenGL routes pass authentic TITLE through Mission 1 state-0 movement
with matching normalized checkpoint fingerprints and final player position.
Human visual acceptance remains the owning gate.

Overlay coverage for these candidate runs is honestly interpreter-backed:
existing caches have stale emitter hashes, and the current broader-capture
preflight has four failed audits. No failing shard set was installed. See
`WIDESCREEN_FLICKER_PROVENANCE_2026-08-04.md` for the evidence, candidate hashes,
three bounded overlay falsification attempts, and test command.

## Objective

Preserve compatibility checkpoint `2009297` on
`experiment/sf2-recomp-feasibility` and the accepted pass-1 checkpoint
`5b64d86` while implementing the explicitly authorized production-quality
16:9 and direct semantic mouse-control pass on
`experiment/sf2-modernization-pass2`.

Mouse motion must no longer emulate retail D-pad/stick input. Independently
verified SCUS-94451 semantic boundaries must consume relative yaw/pitch for
third-person camera control and first-person aiming while physical controllers
remain on the retail PAD path and scripted cameras retain ownership. Native-wide
16:9 must work under software and OpenGL at high internal resolution without
hardcoded movie dimensions, presentation-only cropping, or copied addresses.

The pass ends with an automated Mission 1 gate and a separate enhanced build
ready for a human Missions 1--8 Disc 1 playthrough. The 4:3 compatibility build,
cards, and launcher remain isolated for immediate A/B diagnosis. PGXP and
high-refresh interpolation are outside this milestone, but the new interfaces
must not obstruct either follow-up.

## 2026-08-03 modernization pass 2 start

- Branch: `experiment/sf2-modernization-pass2`, forked cleanly from pass-1
  checkpoint `5b64d86`.
- Compatibility remains frozen at `2009297`; pass 1 remains available at
  `5b64d86` with its separate executable and cards.
- The private corpus and hybrid handoff classify direct mouse camera ownership,
  native-wide world projection, persistent pages, and fullscreen-effect policy
  as confirmed behavioral leads. PGXP/high-refresh/settings parity are still
  open in the hybrid project and are not part of this pass.
- External mouse hook addresses and object offsets remain unverified leads until
  checked against the user-owned SCUS-94451 executable and bounded live traces.
- Initial ignored footprint is 7.196 GiB, below the 20 GiB ceiling.

## 2026-08-03 modernization pass 2 automated gate

- The opt-in build now uses 4x native-wide 16:9 with GTE-derived gameplay
  classification. Clean hidden software and OpenGL routes preserve TITLE,
  24-bit FMV and state-8 briefing as authored 4:3, then present Mission 1
  gameplay from a 512x240 guest-wide surface to full 1920x1080.
- Relative motion bypasses PAD and applies independently scaled, non-inverted
  chase and held-L1 yaw/pitch at a locally verified resident boundary. Retail
  mouse buttons and physical controllers remain PAD input; invalid state,
  changed code and scripted camera ownership fail closed.
- The OpenGL route automatically proves chase and aim mutations plus retail
  state-0 player ownership and movement. The software route passes the same
  authentic retail progression and wide presentation gates.
- Generic automatic cull discovery emits no SF2 site, including with the
  observed 240-line vertical signature. This remains an honest human edge-
  visibility gate, not a hardcoded address.
- Framework tests pass 49/49 and the focused direct-mouse unit passes. The
  enhanced build is ready for the user-controlled Missions 1--8 validation;
  compatibility remains isolated for A/B reproduction.
- The Disc 1 launcher now records a payload-free semantic session: exact PAD
  timeline identity, build/configuration hashes, bounded presentation and
  application transitions, player/camera samples, and cumulative fullscreen-
  rectangle expansions. A separate launcher clones its cards into new frozen-
  baseline state for practical later-mission A/B without sharing writes.
- The recorded launcher now verifies the exact enhanced executable, generated
  configuration, settings, OpenBIOS and frozen 4:3 control against the
  source-owned pass-2 candidate manifest before opening a window. This closes
  the stale-documentation gap that otherwise made a valid human run ambiguous.
- The monitor's unit and invisible lifecycle smoke pass; the registered suite
  is 49/49. Human Missions 1--8 visual/input acceptance remains required and
  is not replaced by these counters.
- Human validation contradicted the provisional Mission 1 outdoor-backdrop
  rule. The first textured OT rank includes projected curved environment
  geometry, so stretching that rank independently corrupts the 16:9 margins
  even though the canonical 4:3 centre remains correct. The production profile
  now disables that transform while the original finite-edge reveal is
  returned to the open-invariant queue.
- The same human evidence showed cinematic top/bottom mattes ending at the
  authored 4:3 edges. The generic effect predicate now treats an axis-aligned
  rectangle or quad spanning both authored horizontal edges as full-output
  width regardless of height. Flat, Gouraud and textured quad forms share the
  rule. The first live software attempt then proved the packet coordinates are
  pre-offset: gameplay authors `-192..+192` and GP0 draw offset maps that span
  to framebuffer `0..384`. The corrected title-neutral predicate accepts the
  authored origin derived from the live draw-area/draw-offset state and blocks
  later HUD re-anchoring of matched effects. Focused centered- and zero-origin
  regressions plus 49/49 framework tests pass. A fresh hidden-window software
  route passed authentic state-0 ownership and movement with 7,035 effect
  expansions at player ownership. The matching hidden OpenGL route passed the
  same gates and expansion count; human visual confirmation remains pending.
- A final-hash interpreter control passed the complete route. Legacy native
  shards failed at the state-8 exit and were discarded. Recompiling only the
  five images captured by that successful control produced a clean native
  repeat through state-0 movement and both mouse modes: 15,831,907 resident
  AOT, 6,862,443 compiled-overlay and 542,932 interpreter dispatches. Fallback
  is 7.3316% of the overlay tier and is not reported as native coverage.
- Automated Mission 1 is therefore green for the exact candidate recorded in
  `DISC1_VALIDATION.md`. Human Missions 1--8 acceptance is now the next gate.
- Human Mission 1 inspection rejected that candidate's OpenGL native-wide
  presentation. The canonical 4:3 centre remained coherent, but the independently
  rasterized reveal margins exposed fan-shaped affine texture distortion and a
  hard discontinuity at both former 4:3 edges. This contradicts visual acceptance
  despite the semantic route passing. The first bounded A/B now disables the
  OpenGL centre-blit optimization (`nw_full_mirror = true`) so the entire wide
  image follows one raster path, matching the software backend architecture.
  This is an unaccepted visual candidate until the user verifies Mission 1; the
  recorded candidate manifest intentionally still names the last automated build.

## 2026-08-03 modernization pass 1

- A separate Release executable is built under ignored `build-modern-pass1`;
  the baseline `build-r8-scheduled-input` executable is untouched. Launchers
  use distinct window titles and memory-card directories.
- The modern profile uses OpenGL 4x internal supersampling, borderless desktop
  4:3 output, nearest texture sampling, and no interpolation or widescreen.
- Keyboard and bounded relative mouse motion translate only to the ordinary
  active-low retail PAD word. There are no guest player/camera writes,
  generated-code changes, callbacks, or state forcing.
- Hidden/unfocused keyboard and mouse input is neutral. The modern launcher
  also disables dev-any-input merging, so unrelated background controllers do
  not perturb its assigned keyboard/mouse profile.
- Two clean hidden OpenGL/dummy-audio runs passed complete authentic startup,
  retail New Game/One Player, Mission 1 state 8, post-FMV dialogue, state-0
  player ownership, and movement. Startup/input hashes and normalized
  fingerprints match at all five checkpoints.
- Final run F/G ownership is resident AOT 15,830,035/15,829,527; compiled
  overlay 143,524,201/143,523,764; interpreter fallback 683,265/683,155.
  Fallback is 0.4738%/0.4737% of overlay-tier dispatch and is not native.
- Both runs have zero lost CD INT1 events, 1,208 SPU key-ons, identical
  nonzero XA totals, and final XYZ `(-5606,2036,7529)`. The ignored footprint
  is 7.137 GiB.
- Detailed handoff: `docs/sf2/MODERNIZATION_PASS1.md`.

## 2026-08-03 overnight closure

- Generic OpenGL split-VRAM ownership is corrected at the 15-bit-to-24-bit
  transition. The backend finishes FBO-owned work into CPU VRAM before packed
  movie uploads, mirrors depth-24 fills into both representations, and orders
  ownership before CPU uploads. The title-neutral regression passes.
- The affected hidden-OpenGL acceptance route changed from 292 CPU/FBO mismatch
  samples to zero; software remained the correct control. The user also
  confirmed the formerly corrupt FMV bands render correctly.
- Missing Eidetic and ZINTRO playback was not an input skip. Exact sector
  history proved that retail issued the new SetLoc/SeekL sequence while the CD
  device continued the old read stream. SeekL/SeekP now cancel ReadN/ReadS and
  pending data-ready ownership before entering SEEK. The title-neutral
  regression passes.
- A clean no-input process now produces the complete authentic sequence at
  deterministic frames: 989 logo 925, Eidetic 1268, legal 1422, ZINTRO 1752,
  TITLE 18493. The compound TITLE predicate remains stable for 60 guest frames
  before any input is armed.
- Exact guest-frame PAD scheduling and emulation-thread applied-input telemetry
  remove host socket timing from automation. The complete framework suite is
  43/43.
- Two final clean native-enabled processes passed the same retail route through
  New Game, One Player, aircraft FMV, state 8, post-FMV dialogue, state-0
  player/camera ownership, and D-pad movement. All scheduled input application
  frames match. Same-frame RAM-write, store-PC, MMIO, scratchpad, and cycle
  fingerprints match at all five semantic checkpoints.
- Both runs load eight overlay regions and expose 573 compiled candidates at
  player control. Final ownership is approximately 15.83 million resident-AOT
  hits, 141.71 million compiled-overlay dispatches, and 0.683 million
  interpreter fallbacks. Fallback is 0.480% of overlay-tier dispatches and is
  not counted as native coverage.
- Both runs finish with zero lost CD INT1 events, more than 1,200 SPU key-ons,
  identical nonzero XA input totals, identical player/camera identity, health
  150, and final XYZ `(-5606, 2036, 7529)` after input.
- The ignored local footprint is 6.604 GiB, below the 20 GiB ceiling.
- Full evidence and handoff: `docs/sf2/OVERNIGHT_REPORT_2026-08-03.md` and
  `docs/sf2/PSX_PORTS_RETURN_2026-08-03.md`.

## 2026-08-03 connected-slice recording follow-up

- The user completed Mission 1 from cold boot through one death/checkpoint
  reload, completion, retail save, and entry into Mission 2. This is retained
  as human functional acceptance.
- The 43,967-frame input artifact is structurally exact but is not accepted for
  deterministic replay. A late low-latency host sample overwrote both the
  recorder's early sample and replayed input. Hidden OpenGL proved the first
  divergence at frame 20,552: timeline `0xFFEF`, SIO `0xFFFF`.
- The generic recorder now owns the true final SIO sampling boundary, covers all
  early-return paths, and keeps replay faults sticky. Unit and source-order
  regressions pass; a rebuilt real two-process preflight records the exact
  frames 240--259 pulse and observes it during replay.
- A single corrected human recording is now required. It must then replay twice
  under hidden OpenGL/dummy audio before the connected slice can graduate.
- Detailed evidence: `PAD_TIMELINE_REPORT_2026-08-03.md`.

## Verified state

- Compatibility branch: `experiment/sf2-recomp-feasibility` at `2009297`
- Active opt-in branch: `experiment/sf2-modernization-pass1`
- Scaffold commit: `83e0d70`
- PSXRecomp baseline: `0cfa9fe0a8da944e9f694a24361b4973c57131ea`
- R0 passes: two corrected-package generations contain 992 identical
  non-build files after exact output-root normalization and both build.
- Final PE products differ only in documented PE/build-ID timestamps and the
  derived checksum; normalized product hashes match.
- Complete framework suite passes 38/38 with `PYTHONUTF8=1`.
- Bundled OpenBIOS LLE identity and loaded checksum match.
- Two clean headless runs reproduce the same boot call chain:
  `0x800F8598` at frame 727, `0x80029624` at frame 727, and
  `0x80029700` at frame 728.
- SF2 installs resident overlay images with ordinary CPU stores into the
  original executable text window. Bounded write evidence identified the copy
  loop at `0x8001076C..0x80010778`, including the TITLE destination around
  `0x8014B950`.
- The generic text-write guard now marks changed executable pages dirty, making
  CPU-installed code eligible for the existing capture-and-compile pipeline.
  Generated C and captured overlays were not edited.
- Corrected capture discovered four regions. The SF2 resident-overlay regions
  are `0x8013E000` (40,964 bytes) and `0x8014B000` (180,228 bytes); the latter
  contains the TITLE image. All four cache shards compile with no failed shard,
  unsupported-instruction TODO, or unknown/bad target.
- Clean cache-reuse runs load four regions and register 117 native candidates.
  Representative post-TITLE queries report about 13.9–15.0 million native
  overlay dispatches, exactly 17,416 interpreter fallbacks, zero static misses,
  zero stale blocks, and zero revalidation CRC misses.
- Retail application state is reproducibly depth 2, state 4, transition 0.
- Two clean processes have identical frame-2500 fingerprints: COP0 state, EPC,
  interrupt registers, all GPRs, display `320x240x15` at page Y=240, digital pad
  `0xFFFF`, and SIO state all match. Bounded GP0 counters also match exactly.
- No retail BIOS path is configured; OpenBIOS LLE is sufficient for the
  measured resident and TITLE boundaries.
- R2 initially exposed a generic CFG-emitter defect, not an SF2 lifecycle
  defect. The captured low-RAM OpenBIOS fasttrack handler uses the MIPS-I
  dependent pair `lw k0,0x4c38(k0); move at,k0`; the successor must observe old
  `k0`. Captured-overlay CFG emission wrote the load result immediately and
  corrupted the kernel CD queue pointer. Candidate-rank bisection isolated
  owner `0x00003590`; interpreter instruction evidence and the existing
  full-function emitter contract independently confirmed the load delay.
- CFG emission now defers ordinary dependent load writeback through the
  immediate successor. A focused regression uses the exact instruction pair.
  The regenerated cache namespace is `cg9_9713afe3_gccd77ebe4`; the four-shard
  preflight and real build both pass.
- The generic two-file fix and regression were extracted onto current upstream
  `master`, passed the 38/38 framework suite, and were submitted upstream as
  [PSXRecomp PR #93](https://github.com/mstan/psxrecomp/pull/93). SF2 lab
  diagnostics, documentation, and private artifacts are not part of the PR.
- Two clean all-native runs passed the former freeze boundary with zero lost
  CD INT1 events. They reached frames 3,861 and 4,055 with 22,085,066 and
  23,973,334 GP0 writes respectively, versus the broken fixed ceiling of
  7,573,945 writes. Native-overlay and interpreter dispatch remain reported
  separately.
- Retail pad injection is now operable when synchronized to TITLE internal
  state 3 at `0x80156BDC`. Active-low START (`0xFFF7`) reaches the retail
  `New Game` menu; Cross (`0xBFFF`) selects `New Game`, then `One Player`.
  The unmodified application stack advances through movie state depth/state
  `3/3` and reaches the state-8 mission briefing at depth 2.
- The first frontend traversal captured and compiled two additional variants:
  `0x8013E000/BA003DC3` and `0x8014B000/979FB883`, with zero failed shards,
  unsupported-instruction TODOs, or unknown/bad targets. A clean reuse run
  loaded six regions and registered 242 candidates. Live candidate CRCs match
  at MOVIE `0x80143A10` and TITLE `0x801538C4`/`0x801501F0`.
- The clean reuse route records an actual replacement lifecycle event before
  state 8: one invalidation, one stale dispatch blocked, and one revalidation
  CRC miss. This is measured replacement behavior, not an inferred initial
  load.
- State-8 fallback attribution is bounded and complete for the measured run.
  Of 268,338 attributed interpreter block dispatches, 44.79% were in
  `0x80141000..0x8014AFFF`, 26.95% in `0x80010000..0x800CFFFF`, 26.75% in
  `0x800D0000..0x8013DFFF`, 1.50% in `0x80158000..0x801DFFFF`, and 0.01% in
  `0x8014B000..0x80157FFF`. The leading uncovered entries are
  `0x80142AE4`, `0x80142BD4`, `0x80142B84`, `0x800F928C`, and
  `0x80022584`; none is covered by a cached code-range manifest. Successful
  shard builds report zero unsupported instructions, so this is missing seed
  coverage rather than unsupported-opcode fallback.
- The invalidated candidate is now identified precisely: old
  `0x8014C0F0/1FC7107E` sees live CRC `4FCFFE46`. A state-8 capture compiled as
  region `0x8014B000/81E32E21` with 360 candidates, plus bounded exact-demand
  fragment `0C2EF971` for the observed stale entry. A fresh process loaded the
  fragment and exposed a valid matching `0x8014C0F0/FF46C59F` candidate beside
  the rejected old candidate, then reached state 8 with zero lost CD INT1.
- Presentation boundaries observed on the continuous route include the
  `320x240x15` title/menu, `512x240x24` opening movie, and `384x240x15`
  mission briefing. CD retains `int1_lost=0`; bounded SPU telemetry records
  4,135 key-ons and active retail voice traffic.
- XA delivery and output are now proven on the retail aircraft movie with a
  non-headless process using SDL's silent dummy host backend. The sector ring
  records realtime audio sectors with submode `0x64`, coding `0x01`, and
  `xa_audio_delivered=1`; the CD-input tap produced 1,481,760 frames, including
  1,238,507 nonzero and 1,206,329 audible-threshold frames. SPU-output and
  host-output taps were nonzero, the audio event ring recorded `CD_PUSH` and
  subsequent `RENDER` events, and CD `int1_lost` remained zero. No audio payload
  was dumped or persisted.
- A visible native-overlay run exposed a reproducible Mission 1 intro failure
  at the first dialogue boundary. It was not an audio wait: frame 4,828 halted
  on the dispatch-recursion guard at depth 257 while the retail descriptor
  trampoline at `0x80108BEC` repeatedly crossed through `0x80010000` to
  `0x8002A094` and overlay callback `0x801C63B4`. A clean
  `PSX_OVERLAY_NATIVE_OFF=1` control reproduced the identical cycle at frame
  10,145, excluding the native overlay DLL/cache as the cause.
- The generic dirty-RAM JALR implementation incorrectly treated every encoded
  link register as a conventional `$ra` call contract and also rewrote `rd=0`
  as `$ra`. SF2 uses `jalr $a1,$t0` for a data-bearing descriptor trampoline:
  `$a1=pc+8` points at the descriptor while the target ultimately returns via
  the pre-existing `$ra`. Non-`$ra` JALR now preserves that architectural
  pc-chain instead of fabricating a `pc+8` continuation. The framework suite
  passes 39/39 with the new source guard.
- With the fix, an interpreter-only automation run remained live beyond the old
  fatal frame, but that frame-number comparison is not counted as a semantic
  dialogue-boundary pass because the startup input gate was ambiguous. The
  decisive validation came from the user controlling a fresh native-overlay-on
  process through `New Game` and Mission 1: the retail intro crossed the exact
  former dialogue boundary and entered live `384x240x15` gameplay. The bounded
  post-boundary query at frame 18,191 reported 98,979,844 native-overlay
  dispatches, 313,042 interpreter fallbacks, one real invalidation/stale block,
  ongoing world rendering, and CD `int1_lost=0`.
- That validation process intentionally used SDL's dummy audio driver. Internal
  CD/XA, SPU, and host-buffer taps were nonzero, but the user correctly observed
  no physical sound. Audible real-device output remains separately unvalidated.
- The first TITLE-state probe was not globally unique: `0x80156BDC == 3` also
  appeared around frame 945 while startup presentation was still in a 24-bit
  movie phase. Injecting START there can explain the observed missing Eidetic
  and `z_intro` presentation. Those omissions are therefore recorded as an
  input-harness confound, not yet as a retail/framework presentation defect.
- A real-device run confirmed audible retail FMV/dialogue output. The same run
  reproducibly hung in `Save and Quit` before the first memory-card write even
  though complete card reads, BIOS events, and SIO IRQ acknowledgement were
  healthy. The apparent callback target `0x80145360` was rejected as the cause:
  it is an exact compiled candidate, executes natively, and returns normally.
- The save hang was a generic scheduler invariant violation. A native call unit
  incremented `g_call_unit_depth`, and both overlay cycle-interrupt wrappers
  suppressed every IRQ while that depth was nonzero. Retail's event pump can
  wait inside such a call for the next IRQ-backed BIOS event, so the call could
  not return until an IRQ that the call itself permanently suppressed.
  Nested call units now deliver IRQs; the existing interrupt layer continues to
  preserve atomicity by restoring the interrupted thread and deferring only a
  requested cooperative cross-thread switch to the clean outer boundary.
- The focused source regression and complete framework suite pass 40/40. A
  rebuilt clean executable reached the retail frontend headlessly, then the
  user repeated the exact visible save route with native overlays and real
  audio. `Save and Quit` completed, returned normally, and the resulting save
  loaded successfully. The retained SIO transaction ring contains 849 closed
  transactions, including 120 successful `0x57` writes; the resulting card is
  a valid 128 KiB image with active directory entries. This closes the save
  deadlock without changing SIO timing, card formatting, persistence, retail
  state, generated C, or captured overlays.
- The successful load exposed a separate presentation defect in 24-bit FMVs:
  the decoded movie rectangle is correct, but stale/corrupted VRAM is visible
  in the bands behind it. Treat this as a display/upload-coverage issue, not
  MDEC stream corruption. Bounded telemetry now proves a `512x240` depth-24
  scanout at `(0,0)` while each movie buffer is assembled from 32 `24x160`
  strips covering `x=0..767`, centered at `y=40..199` or alternate-buffer
  `y=280..439`. The identical executable/save is correct under the software
  renderer, isolating the defect to OpenGL CPU/FBO VRAM coherency. OpenGL fills
  update only its authoritative FBO, while depth-24 presentation reads packed
  RGB888 from the CPU mirror and cannot blanket-sync after the movie upload.
  The generic correction remains open.

## R1 verdict

- Passed on 2026-08-02. Retail state 4 is reached with the TITLE shard active,
  native-overlay/cache and interpreter tiers measured separately, and the
  fixed-frame guest/input/presentation boundary reproduced twice.
- Evidence is in `docs/sf2/devlogs/2026-08-02-recomp-bring-up.md`.

## Next execution sequence

1. Perform the short user-visible OpenGL acceptance check on the retained r8
   build: complete startup, clean FMV bands, and Mission 1 control. This is
   presentation confirmation, not a substitute for the completed headless
   semantic gates.
2. Select Mission 3 only through retail-owned menus and add its semantic
   checkpoints under the existing comparison protocol.
3. Run Mission 3 twice from clean processes and report overlay convergence and
   fallback separately.
4. Prepare generic GPU, CD seek, and deterministic-input changes for upstream
   review only with explicit user authorization.

## R2 remaining target

- R2's reproducible Mission 1 route is closed.
- The remaining feasibility-plan target is the representative Mission 3 route
  through retail-owned frontend state, without modernization or native gameplay
  substitutes.

## Known environmental detail

Windows is using a Greek legacy code page. Python source-reading tests can fail
with `cp1253` decoding errors unless `$env:PYTHONUTF8 = "1"` is set. This is an
environment issue, not a framework regression.

## Modernization pass 2 visual gate

Human A/B testing accepted `nw_full_mirror = true` as correcting the distorted
native-wide margins, but exposed genuine edge culling. The read-only hybrid
oracle identifies the owning invariant as guest-visible GTE horizontal
projection. The new opt-in preserves widened guest visibility and unsquashed
raster X through exact SWC2-to-GP0 address/value provenance, leaving
CPU-authored HUD and effects unchanged. Focused config and GTE provenance tests
pass. Human evidence then narrowed the remaining rectangular loss to the finite
outdoor sky/SCRIM boundary rather than whole-scene frustum culling. The current
A/B adds only the existing edge-only textured transform: interior/canonical
vertices remain unchanged and only already-outside vertices expand toward the
wide edge. Human evidence rejected that A/B: the black SCRIM boundary remained,
and Gabe appeared horizontally thinner. Both guest projection and textured-edge
expansion are disabled in the current build, returning to the accepted
full-mirror presentation while the semantic SCRIM owner is investigated.

Further human evidence proves NPCs also disappear at the wide edge while the
floor remains continuously submitted. A third bounded candidate restored the
widened guest projection and replaced incomplete SWC2 provenance with
ordering-table compensation. Human testing rejected it: culling persisted and
character proportions were wrong again. The active generated profile is
therefore restored to the aspect-correct full-mirror-only baseline.

Corpus consultation plus three distinct bounded falsification attempts are now
complete: exact SWC2-to-GP0 provenance was incomplete, edge-only textured
expansion did not affect the loss, and world-ordering-table inverse compensation
changed aspect without retaining culled objects. The first unresolved semantic
divergence is that actor and finite-background packets cease to be submitted
inside the added field while resident floor geometry remains. The next work is
an evidence capture of the retail model/room visibility owner, not another
presentation transform.

The restored Release executable is
`F47B337D391BA44CE57B436E5B739CB27DE47A3EC8E2E7DA67AE29544D5E586C`;
its generated config is
`06D2C40300CE7AC015A5E5C52D06B726463EB781485A87F61DF6D846A87DB2DF`
and contains only `nw_full_mirror = true` among the three rejected A/B flags.
`tools/start_sf2_widescreen_cull_capture.ps1` now arms the bounded packet
census and captures the preceding 360 frames when the human tester holds a
camera at the disappearance boundary. It records GPU and semantic state plus
input identities under the ignored local tree and never writes guest state.

The later value-provenance attempt is also rejected. Live evidence showed more
than 1.15 million matches and severe rhythmic corruption; equality of packed
SXY values does not preserve primitive ownership across DMA submissions or
frames. A retail-4:3 live A/B removed that corruption and restored proportions,
while the original edge culling returned.

Read-only DMA traversal at the held Mission 1 position now identifies the
missing semantic boundary: six linked-list submissions occur per gameplay
tick, with three setup-only lists, one dense 696-polygon world list, then 31-
and 10-draw auxiliary/UI lists. The active candidate work moves inverse
projection to that dense world-submission boundary and extends the bounded
census with list ordinal/root evidence. This directly mirrors the verified
hybrid ownership rule without copying an address or classifying individual
triangles by reused coordinate values.

## 2026-08-05 — transform-snapshot high-refresh R3 awaiting visual verdict

The SF1-style semantic transform path now retains authentic pre-world render
state and replays renderer-native geometry without advancing retail execution.
Exact GTE transform provenance, mutual object-space group matching, camera-cut
rejection, immutable raw-texture capture, atomic whole-primitive projection,
and separate Y=0/Y=240 display-page snapshots are implemented. HUD, FMV,
briefing, fades, fullscreen effects, ambiguous groups, and unsafe projections
fail closed to the accepted current frame.

The latest automated Mission 1 route passed with guest VBlank 60.98 Hz, retail
world updates 18.99 Hz, and actual swaps 60.48 Hz. Both page slots were valid,
alpha=1 reconstruction was pixel-exact 2/2 with zero differing pixels, 408/408
eligible midpoint vertices moved, 320 repeated midpoint vertices agreed, and
there were zero seam mismatches, render failures, or overflows. Evidence is
ignored under `evidence/high-refresh-transform-twopage-fix-20260805-132105`.

An earlier single-snapshot output produced major white flashes despite passing
cadence and endpoint gates. It is rejected. Root causes were partial-primitive
projection and failure to retain independent snapshots for SF2's vertically
stacked double-buffer pages. The corrected human-test shortcut is
`Launch SF2 Transform High Refresh R3.bat`; qualification remains blocked on
the user's visual verdict, especially flashes, texture seams, perceived speed,
HUD stability, and motion smoothness.
