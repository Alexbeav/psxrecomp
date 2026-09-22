# PGXP pass 1 — accepted checkpoint

Date: 2026-08-04

## Result

PGXP pass 1 is accepted on `experiment/sf2-pgxp-pass1`. It layers visual-only
precision over the approved native-wide and direct-mouse build without changing
retail-visible GTE registers, RAM stores, canonical PS1 VRAM, gameplay, timing,
or authored state.

Human Mission 1 A/B testing reports correct aspect, no widescreen culling, and
a substantial reduction in polygon wobble and texture swimming. This is not a
claim that every visible primitive has PGXP provenance or that every possible
source of motion instability is eliminated.

## Ownership contract

1. GTE projection retains precise 16.16 X/Y and depth beside an exact SWC2 RAM
   address and generation.
2. GP0 packet assembly queries each triangle vertex by packet address and
   packed value.
3. A triangle receives precise positions only when all three vertices match.
4. A textured triangle receives reciprocal-depth interpolation only with the
   same complete match.
5. A partial, stale, invalid, CPU-authored, address-mismatched, or
   packed-mismatched triangle falls back atomically to native PS1 coordinates
   and affine mapping.
6. Software and OpenGL consume one renderer-neutral `GrPrecisionTriangle`.
7. Canonical guest-visible VRAM always uses native affine rasterization; PGXP
   affects only high-resolution/presentation mirrors.

The implementation contains no SF2 address, generated-retail-code edit,
fabricated callback, state force, or native gameplay substitute.

## Accepted build identities

- OpenGL executable:
  `CA6CE21CB71CE71C6A270939BCD12752FCFC0FF224B7761B94F93DC31FD87DC3`
- OpenGL generated configuration:
  `80BA2A4D702131EA5288B5EB12F39848C64AFFD9370E4737BEA389F1CE08FE45`
- Software executable:
  `99E349462B1E5DDD4D11626A459A41EC82BCFA5320408035744C06809F16E594`
- Software generated configuration:
  `A1579B1F56DA2A3B64F8425A234D3B7BD3AB0E7B3E4911ABEE2E4A25139BF655`

The ignored build names are `build-pgxp-pass1-widescreen-r2` and
`build-pgxp-pass1-software-r2`. The accepted desktop test shortcut points to
the OpenGL build. The frozen 4:3 and non-PGXP widescreen controls remain
separate.

## Validation

- CLI Release generation and link: pass.
- Registered suite: 50/50 pass with `PYTHONUTF8=1`.
- Focused `pgxp_native_vram_test`: pass.
- Hidden-window OpenGL Mission 1 route A: pass in 437.2 seconds.
- Hidden-window OpenGL Mission 1 route B: pass in 437.6 seconds.
- Existing evidence comparator: pass with no errors.
- Hidden-window software Mission 1 route: pass in 451.4 seconds.
- Frozen 4:3 OpenGL compatibility executable: bounded hidden-window launch
  passes with widescreen unconfigured and inactive.
- Human OpenGL Mission 1 visual/controls evaluation: accepted.
- Ignored local footprint after qualification: 14.836 GiB, below 20 GiB.

The matching OpenGL pair has startup hash
`6bf9af29fcb41a118455be11a25893937f59e64e3d2bb173bc58f937d4e1bff2`
and input-schedule hash
`c518cd5e1e597e70eebc0e82e8b305dcc56f6499f8672a52867cdc89bdefd650`.
Normalized fingerprints match at stable TITLE, aircraft FMV, state 8, player
ownership, and movement. Both finish with retail player/camera ownership,
health 150, and XYZ `(-5606,2036,7529)` after the same frame-25800 PAD pulse.
Both observe 1,200 SPU key-ons, identical nonzero XA input, and zero lost CD
INT1 events.

The software route passes the same semantic gates and its normalized checkpoint
fingerprints match OpenGL. Its polling loop first observed `LEGAL.STR` at frame
1124 rather than 1105; all other startup identities and observed frames match.
This host/backend observation difference is retained rather than normalized
away.

## Measured precision and execution ownership

At the final OpenGL checkpoint, representative counters are:

- 390,433 submitted triangles;
- 14,823 complete exact PGXP triangles;
- 375,610 unmatched triangles;
- zero partial, stale, address-mismatched, packed-mismatched, or invalid
  triangles at that sample.

The total includes UI and other non-world submissions, so the exact ratio is
not a percentage of visible world geometry. It does prove that pass-1 coverage
is selective and explains why some motion-era instability can remain.

Execution ownership is also incomplete by design in this checkpoint. The
copied overlay cache has an older codegen hash, so both accepted OpenGL routes
measure approximately 15.99 million resident-AOT calls, zero compiled-overlay
dispatches, and 13.53 million interpreter fallbacks. Interpreter execution is
not native coverage.

## Rejected and narrowed observations

- The first user candidate reintroduced culling because its generated profile
  accidentally set `nw_guest_projection = false`. Comparing exact generated
  configurations identified the error; rebuilding with the accepted projection
  switch removed culling and improved the PGXP result.
- A no-window `--headless` qualification attempt was invalid for presentation
  features because that frontend intentionally bypasses SDL/GPU initialization.
  It stalled in state 8 with widescreen unconfigured. The valid CI mode is
  `--hidden-window`, which instantiates the selected renderer invisibly.
- Cross-renderer movie observation time is not treated as an exact deterministic
  oracle. Same-renderer clean-process comparison owns the exact startup gate;
  normalized guest fingerprints own the renderer comparison.

## Next milestone

1. Rebuild or recapture the active overlay set for the current codegen/config
   hash and measure resident, compiled-overlay, interpreter, and renderer host
   time separately.
2. Freeze this checkpoint as the PGXP-on oracle and keep the accepted non-PGXP
   widescreen and 4:3 builds available for A/B.
3. Create an isolated 60 Hz presentation-interpolation branch and build.
4. Measure retail simulation, GPU submission, display-buffer selection, and
   host presentation as separate timing domains.
5. Interpolate only when complete PGXP provenance and compatible scene/display
   continuity exist; fail closed on UI, FMV, cuts, teleports, and missing data.
6. Keep retail simulation, timers, CD, SPU/XA, PAD sampling, scripts, and
   authored timing unchanged.

Tenchu remains the first independent consumer for the generic PGXP contract.
Prepare a payload-free corpus return after provenance and license review; do
not include retail data or local evidence artifacts.
