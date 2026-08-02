# SF2 hybrid-versus-recompilation comparison protocol

## Purpose

Compare architecture, correctness, determinism, coverage, and engineering cost
without relying on subjective impressions of a playable scene.

## Fixed comparison points

Use the same retail revision and equivalent input routes at these boundaries:

1. Executable entry and CRT completion.
2. First `Game_Main` application-loop iteration.
3. Stable TITLE state.
4. TITLE-to-Mission-3 transition.
5. First authored Mission 3 in-engine frame.
6. Player-control handoff.
7. Truck equipment objective completion.
8. Death and retail checkpoint restart.

## Record at every point

- framework and game-project commit;
- input executable and disc identity hashes;
- guest PC, SP, GP, RA and application state;
- stable hashes of selected guest RAM regions;
- active overlays with load addresses, sizes, and hashes;
- native-recompiled, native-overlay, and interpreter dispatch counts;
- GPU submissions, display-page state, and frame hash where meaningful;
- SPU/XA/CD state and retail clocks;
- wall time, CPU time, frame pacing, and peak memory;
- warnings, fallback paths, unsupported operations, and host patches; and
- the exact command and deterministic input artifact used.

Never compare raw host pointers, timestamps, randomized temporary paths, or
other values that cannot be stable across runs.

## Determinism gate

Run each automated route at least twice from a clean process. Normalize only
explicitly documented host-variant fields. Hash the remaining trace and fail
the gate if it differs.

A rendered frame, changed geometry hash, or accepted input is not proof of
playability. The Mission 3 gate requires evidence that the retail state reaches
player ownership and that player position changes under the recorded input.

## Fairness rules

- The hybrid runtime may supply expected values but must not be modified to
  make the recompilation result look closer.
- Count every SF2-specific patch, hook, address, config rule, and exception in
  both implementations.
- Distinguish generic framework fixes from game-specific fixes.
- Report interpreter fallback honestly; working through fallback is not static
  recompilation coverage.
- Compare the retail-compatible profile, not SF2 Modern enhancements.
- Preserve failed experiments and rejected hypotheses in a chronological log.

## Decision table

| Dimension | Hybrid runtime | Recompilation lab |
|---|---:|---:|
| Time to TITLE | Record | Record |
| Time to player control | Record | Record |
| SF2-specific source/config lines | Record | Record |
| Resident native coverage | N/A or measured | Record |
| Overlay native coverage | N/A or measured | Record |
| Interpreter share | Record | Record |
| Deterministic routes passing | Record | Record |
| CPU/GPU/audio correctness gaps | Record | Record |
| Campaign/save/multi-disc evidence | Record | Record |
| License suitability by audience | Record | Record |

No single performance number decides the outcome. Debuggability, correctness,
coverage convergence, licensing, and the effort required for the next game are
equally important.
