# PSX port knowledge report

- Date: 2026-08-04
- Project: Syphon Filter 2 PSXRecomp feasibility/modernization lab
- Repository/branch/commit: `SF2-Recomp-Lab`,
  `experiment/sf2-modernization-pass2`, documentation checkpoint after `a2b951c`
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
widescreen and direct mouse control in the exercised Mission 1 scenes. PGXP,
high-refresh presentation, campaign breadth, and native overlay convergence are
not complete and must not be claimed.

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

## Shared findings consumed

| Finding ID | Status | Evidence |
|---|---|---|
| `PSX-GPU-004` | independently verified/narrowed | one projection owner is required; final ownership is linked-list submission structure, not reused SXY values |
| `PSX-GPU-005` | lead for next phase | complete address/generation PGXP provenance and native fallback contract selected; implementation pending |
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
  composition and hardware primitive-rejection focused units
- Dispositions: projection-owner rule confirmed and narrowed to DMA submission;
  Tenchu PGXP design is a bounded lead; completed SF2 PGXP/60 FPS code is
  contradicted; VSync-only unlock contradicted

## Reusable artifacts

- `runtime/include/ws_projection_compose.h`
- `runtime/tests/test_ws_projection_compose.c`
- `runtime/tests/test_gpu_primitive_reject.c`
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
| PGXP absent | renderer/GTE metadata | affine texture/vertex instability remains | disabled | software/OpenGL Mission 1 parity and provenance counters |
| 60 Hz absent | presentation/timing | retail frame cadence | no VSync hack | interpolated 60 Hz with unchanged simulation |
| overlay fallback high | loader/capture pipeline | performance ceiling | reported honestly | converged current-hash overlays and host-time profile |
| campaign incomplete | validation | unknown later-mode defects | separate compatibility oracle | consecutive campaign completion |

## Current blockers

No blocker remains for the accepted widescreen/direct-mouse checkpoint. The next
first unresolved invariant is complete PGXP vertex provenance through primitive
assembly without changing guest-visible state.

## Next decisive experiment

Create a title-neutral SWC2 address/generation provenance unit that assembles
complete, partial, stale, CPU-authored, and UI primitives. Pass only if complete
world primitives receive renderer-neutral precision metadata and every other
case uses bit-identical native PS1 coordinates.

## Knowledge-base actions

- Project report updated with the accepted modernization checkpoint
- New stable registry row: none; submission ownership remains a project-level
  candidate pending independent validation
- Next independent consumer: SF3 recomp for linked-list projection ownership;
  SF2 is the independent consumer of Tenchu's PGXP contract
- Upstream candidate: title-neutral projection composition and regression after
  another project validates the submission-owner model
