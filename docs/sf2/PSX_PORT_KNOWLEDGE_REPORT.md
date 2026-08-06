# PSX port knowledge report

- Date: 2026-08-04
- Project: Syphon Filter 2 PSXRecomp feasibility/modernization lab
- Repository/branch/commit: `SF2-Recomp-Lab`,
  `experiment/sf2-pgxp-pass1`, accepted PGXP pass-1 checkpoint
- Retail identity/revision: USA Disc 1, SCUS-94451; supported executable SHA-256
  `75A360BF7465DFDEC85C14F9BA93862AAE2531B48D83FD8D82BA8C9FFFA13D33`
- Architecture lane: resident AOT plus compiled overlays plus bounded interpreter
  fallback; optional presentation/input enhancement profile
- License/provenance boundary: PolyForm Noncommercial 1.0.0; no retail payload,
  generated game code, capture, BIOS, media, screenshot, or save artifact is
  committed

## Executive state

Authentic retail startup, frontend, Mission 1 selection/loading, post-FMV
dialogue, player control, saving/loading, and a human Mission 1 completion have
been demonstrated. The separate enhanced build is human-accepted for native
widescreen, direct mouse control, and PGXP pass 1 in the exercised Mission 1
scenes. PGXP coverage is selective rather than complete. High-refresh
presentation, campaign breadth, and native overlay convergence are not complete
and must not be claimed.

## Product graduation state

- Current state: `representative_slice_verified`
- Evidence supporting current state: authentic connected Mission 1 route,
  authoritative player movement, GPU/SPU/XA/PAD activity, retail save/load, and
  human end-to-end Mission 1 completion; accepted enhanced A/B checkpoint
- Required next state: `campaign_complete`
- Missing hard gates: consecutive Missions 2 through finale; broader scopes,
  NVG, fades, mattes and disc transitions; repeated PGXP/high-refresh routes;
  Release performance ownership
- Human-completion coverage: Mission 1 end to end, checkpoint reload, save, and
  transition to the start of Mission 2

## Verified milestones

| Boundary | Evidence | Repeated? | Human confirmed? |
|---|---|:---:|:---:|
| Authentic startup to stable TITLE | semantic route and startup identities | yes | yes |
| Retail Mission 1 state 8 to state 0 | ownership, movement and device evidence | yes | yes |
| Save/load | retail memory-card transaction path | yes | yes |
| Mission 1 completion | recorded human route | no | yes |
| Native widescreen/direct mouse | isolated enhanced build plus A/B controls | automated route plus iterative A/B | yes |
| PGXP pass 1 | exact SWC2 provenance and renderer-neutral metadata | two matching OpenGL routes plus software control | yes |

## Shared findings consumed

| Finding ID | Status | Evidence |
|---|---|---|
| `PSX-GPU-004` | independently verified/narrowed | one projection owner is required; final ownership is linked-list submission structure, not reused SXY values |
| `PSX-GPU-005` | independently verified/implemented | exact address/generation provenance, complete-triangle ownership, renderer-neutral consumers, and atomic native fallback pass focused and route gates |
| `PSX-TIME-002` | confirmed constraint | VSync-divisor changes rejected as a 60 FPS strategy; four timing domains must be measured |

## New generic candidates

Projection compensation must respect primitive/submission ownership. Packed
screen-coordinate equality is insufficient because values recur across frames
and DMA submissions. For a title that mixes separately owned linked lists, a
bounded semantic submission classifier can preserve world projection while
leaving setup, effects, and UI native. Minimal regression: several linked lists
with known polygon densities, identity in 4:3/disabled modes, inverse composition
only for the classified list, and PS1 primitive rejection on raw packet data.

## Rejected hypotheses

- stretching the presentation surface owns retail edge culling;
- one stale display page owns rhythmic flicker;
- the first textured ordering-table rank is the backdrop owner;
- far-depth or one exact GTE caller owns the missing scene;
- packed SXY value equality proves a primitive came from the current GTE result;
- changing the VSync divisor is sufficient for correct 60 FPS gameplay.

## Corpus consulted for the current blocker

- Search terms/symptom/owner: PGXP, SWC2, address generation, projection owner,
  interpolation, VSync divisor, simulation cadence, interpreter performance
- Findings/failures: `PSX-GPU-004`, `PSX-GPU-005`, `PSX-TIME-002`, GPU and
  presentation contract, performance contract, regression ledger, SF2 hybrid
  and Tenchu project reports
- Existing regressions run: all 50 registered framework tests; projection
  composition, hardware primitive-rejection, and canonical-VRAM PGXP focused
  units; two matching OpenGL routes plus one software route
- Dispositions: projection-owner rule confirmed and narrowed to DMA submission;
  Tenchu PGXP contract is independently consumed by accepted SF2 pass 1;
  completed full-coverage SF2 PGXP remains contradicted; VSync-only unlock
  contradicted

## Reusable artifacts

- `runtime/include/ws_projection_compose.h`
- `runtime/tests/test_ws_projection_compose.c`
- `runtime/tests/test_gpu_primitive_reject.c`
- `runtime/tests/test_pgxp_native_vram.c`
- renderer-neutral `GrPrecisionTriangle` sidecar and bounded PGXP telemetry
- bounded linked-list census and human capture scripts under `tools/`
- separate enhanced/control build profiles and candidate-identity guard

## Performance evidence

- Reference hardware/OS: current Windows workstation; detailed CPU/GPU pending
- Build/profile and renderer: Release software/OpenGL semantic routes
- Exact route: authentic cold startup through Mission 1 state-0 movement
- Ownership: prior clean candidate approximately 15.99M resident-AOT, zero
  compiled-overlay, and 13.54M interpreter fallback dispatches; not native
  overlay coverage
- Host-time/frame tails/top fallback blocks: not yet measured
- Diagnostic overhead: bounded probes only; host-time exclusion not yet measured

## Quality debt

| Debt | Owner | User impact | Evidence/containment | Removal or acceptance gate |
|---|---|---|---|---|
| PGXP coverage selective | GTE/RAM provenance propagation | some motion instability can remain | exact full-triangle match or native fallback | broaden coverage with focused provenance classes and preserve semantic parity |
| 60 Hz absent | missing semantic world ownership | retail frame cadence | R1/R2/R3 renderer-only families rejected | matching decomp or equivalent complete camera/object/bone snapshot and rebuild boundary |
| overlay fallback high | loader/capture pipeline | performance ceiling | reported honestly | converged current-hash overlays and host-time profile |
| campaign incomplete | validation | unknown later-mode defects | separate compatibility oracle | consecutive campaign completion |

## Current blockers

No blocker remains for the accepted widescreen/direct-mouse/PGXP pass-1
playthrough checkpoint. High-refresh presentation is explicitly blocked on
semantic world-state recovery; overlay convergence remains a separate
performance/coverage task and is not a prerequisite for playing the accepted
build.

## Next decisive experiment

Begin a targeted matching decompilation of the world-update/camera/actor-render
path, using the existing overlay/range manifests as function boundaries. Resume
60 Hz work only when immutable previous/current semantic transforms and a
per-display world rebuild can be owned explicitly. Overlay-cache convergence
can proceed independently as a performance packaging task.

## Knowledge-base actions

- Project report updated with the accepted modernization checkpoint; private
  corpus return committed and pushed as `6fd986a`
- Accepted PGXP pass-1 payload-free return prepared in `PGXP_PASS1.md`; private
  corpus publication remains a separate provenance-reviewed action
- New stable registry row: none; submission ownership remains a project-level
  candidate pending independent validation
- Next independent consumer: SF3 recomp for linked-list projection ownership;
  SF2 is the independent consumer of Tenchu's PGXP contract
- Upstream candidate: title-neutral projection composition and regression after
  another project validates the submission-owner model
- High-refresh corpus return: R1/R2/R3 negative controls recorded as
  `PSX-TIME-003` through `PSX-TIME-006` and `FAIL-028` through `FAIL-030`;
  R3 has no upstream code patch because its source architecture was rejected
