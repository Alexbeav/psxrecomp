# Next enhancements — PGXP and high refresh

Date: 2026-08-04

## Decision

Implement PGXP first, preserve the accepted compatibility/widescreen checkpoint
as an oracle, then add 60 Hz presentation interpolation. Do not begin by changing
the retail VSync divisor or running the simulation twice as often.

## Phase 1 — PGXP precision path

Status: accepted first-pass checkpoint on 2026-08-04. The exact SWC2
address/generation sidecar, renderer-neutral triangle metadata, atomic native
fallback, software/OpenGL consumers, provenance counters, and canonical-VRAM
regression are implemented. Two clean OpenGL Mission 1 routes compare exactly
under the semantic evidence tool, an independent software route passes, and
human widescreen testing accepts the visual result. Coverage remains selective,
so broader provenance propagation is future PGXP refinement rather than a
prerequisite for beginning the separately isolated high-refresh pass.

The applicable corpus contract is `PSX-GPU-005`:

1. Preserve every retail-visible GTE register and RAM store exactly.
2. Capture higher-precision projection metadata beside the exact SWC2 store
   address and generation; never key ownership only by a packed screen value.
3. Carry renderer-neutral metadata through GP0 assembly. A primitive uses PGXP
   only when every required vertex has complete provenance; UI, CPU-authored,
   stale, and partial matches use native PS1 coordinates.
4. Consume the same metadata in software and OpenGL. Add counters for complete,
   partial, stale, unmatched, and fallback primitives.
5. Validate 4:3 compatibility, accepted widescreen, software, and OpenGL as four
   independent A/B axes before making PGXP a launcher option.

Tenchu is the first independent oracle: its corpus report contains an exact
16.16/address-keyed SWC2 depth path and OpenGL reciprocal-W consumption, but its
visual/match acceptance is still pending. SF2 should consume the generic
contract, not copy title addresses or assume Tenchu's completeness.

Pass gate: a clean Mission 1 route has identical retail state, input/device
clocks, overlays, CD/SPU/XA state, and normalized semantic hashes with PGXP off
and on; PGXP-on software/OpenGL images show stable geometry with bounded
provenance counters and zero guest-state mutation.

## Phase 2 — 60 Hz presentation

Status: next feature milestone after current-hash overlay convergence and a
Release host-time profile. Keep PGXP pass 1 frozen as the visual/semantic
oracle and create a separately named build and branch for the first
interpolation candidate.

The applicable corpus warning is `PSX-TIME-002`: VSync(2) to VSync(1) can double
fixed-step gameplay. Measure four domains separately before changing policy:

- retail simulation updates;
- GPU submission cadence;
- guest display-buffer selection;
- host presentation cadence.

First interpolate presentation to 60 Hz while retail simulation, timers, CD,
SPU/XA, PAD sampling, scripts, and authored timing remain unchanged. Prefer
PGXP-space transforms/depth for interpolation and fail closed on UI, FMV,
teleports, cuts, missing provenance, and incompatible display transitions.

Only investigate a true 60 Hz simulation mode after identifying SF2's
authoritative timestep and proving route invariance. A faster-looking game is
not a pass if movement, AI, scripts, animation, audio, or device clocks advance
twice as fast.

Pass gate: two clean routes match the compatibility oracle at semantic
checkpoints while host presentation reaches the requested cadence without
simulation acceleration, duplicate input consumption, audio drift, or frame-
pacing tails attributable to interpreter fallback.

## Performance prerequisite

Profile Release builds and report resident AOT, compiled-overlay, and
interpreter fallback host-time shares separately. The last clean modernization
route used substantial interpreter fallback and cannot support an honest 60 FPS
performance claim until the top fallback blocks and renderer costs are measured.

## Order of execution

1. Freeze and tag the accepted widescreen/direct-mouse checkpoint.
2. Add renderer-neutral PGXP metadata plus focused regressions.
3. Prove PGXP on the authentic startup and Mission 1 route in software/OpenGL.
4. Measure SF2's four timing domains and Release performance ownership.
5. Add 60 Hz presentation interpolation with fail-closed scene boundaries.
6. Consider true simulation-rate work only if the title's timestep contract is
   identified and all semantic invariance gates pass.
