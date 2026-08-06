# High-refresh pass 1 — shutdown/resume handoff

Started: 2026-08-04. Last updated: 2026-08-06. This file is chronological —
newest sections are appended at the end. Current status: R1, R2, and both R3
builds are visually REJECTED. Transform replay is retired; the accepted PGXP
baseline remains current. See `CURRENT_OBJECTIVE.md` for the live state.

## Objective and current gate

Branch: `experiment/sf2-high-refresh-pass1`, based on accepted PGXP source
checkpoint `2eebc41`.

The target remains fail-closed 60 Hz presentation interpolation with retail
simulation, timers, CD/SPU/XA, PAD sampling, scripts, and authored timing left
unchanged. No interpolation code has been written yet. Current-hash compiled
overlay ownership must first pass the existing deterministic Mission 1 route.

## Current worktree

Tracked/untracked source work is intentionally uncommitted:

- `tools/compile_overlays.py` and its focused additive-history tests load and
  union the current manifest, `<capture>.d/*.json`, and the legacy case where
  `<capture>.d` is one JSON file.
- `runtime/src/overlay_capture.c` migrates that legacy file to an additive
  directory without overwriting it. The exact old file remains as a
  `.legacy-<fnv>.json` sibling until a later explicit cleanup decision.
- `runtime/src/crash_trace.c` and `runtime/include/overlay_loader.h` expose the
  bounded always-on native overlay ring in ordinary atexit/crash reports.
- `recompiler/CMakeLists.txt` registers the two new source-invariant guards.

Focused validation passed:

```powershell
$env:PYTHONUTF8 = '1'
ctest --test-dir recompiler\build-cli `
  -R '(capture_history|crash_overlay_ring_guards|overlay_capture_legacy_history)' `
  --output-on-failure
python -m unittest `
  tools.test_compile_overlays_additive.AdditiveCaptureTests.test_unions_evidence_but_preserves_reused_address_variants `
  tools.test_compile_overlays_additive.AdditiveCaptureTests.test_loads_legacy_single_file_history_without_rewriting_it `
  tools.test_compile_overlays_additive.AdditiveCaptureTests.test_malformed_record_does_not_discard_valid_sibling
git diff --check
```

The ignored local footprint is 17.091 GiB, below the 20 GiB limit.

## Isolated builds

- Accepted PGXP source/config control:
  `lab/sf2/local/generated-disc1-r2-load-delay/build-pgxp-pass1-widescreen-r2`
- Current diagnostic build:
  `lab/sf2/local/generated-disc1-r2-load-delay/build-high-refresh-pass1-diag`
- Diagnostic executable SHA-256:
  `85072BD189D0F844F4D0B3C732C523EB7AC61B582164127936E59F6025B03205`
- Accepted generated-config SHA-256 remains:
  `80BA2A4D702131EA5288B5EB12F39848C64AFFD9370E4737BEA389F1CE08FE45`

The historical accepted executable (`CA6CE...`) was accidentally rebuilt in
place while proving which source tree the generated CMake project consumed.
It was regenerated from detached source `2eebc41` and the unchanged accepted
config, but the Windows link is not byte-reproducible; the reconstructed
executable is `DE284A5...`. Do not rebuild that accepted directory again.
The detached source is retained locally at
`lab/sf2/local/framework-source-2eebc41` for provenance.

## Proven current-hash ownership

Correct namespace:

```text
cache/SCUS-94451/gcc/win-x64/cg9_82e77d3e_gcd6e97ca6
```

It contains 384 ABI-`0x15` DLLs. The compiler produced usable shards plus 44
fail-closed diagnostics; this is not a zero-failure cache claim. A normal clean
run reached 443 registered candidates and 12 regions with approximately:

- 145.6 million compiled-overlay dispatches;
- 97.8 thousand interpreter fallbacks;
- zero invalidations or stale blocks;
- zero candidate/range-index overflow.

The run exits at frame 24003 with PC zero while leaving the retail state-8
briefing. The route reports `pad_status`/`frame: unknown error` only because the
runtime has already exited. This reproduces from clean processes.

## Bounded falsification record

### Control A — all interpreted

Passes authentic startup, state 8, player ownership, and state-0 movement.
This confirms the captured retail route and devices are healthy and narrows the
owner to compiled execution.

### Control B — block final entry `0x8000293C`

Contradicted as a single-entry explanation. It exits at frame 24003 and changes
the report's unreturned owner to `0x80002954`. Both entries alias the same
compiled code range with CRC `3F64E67F`.

Evidence:
`lab/sf2/local/generated-disc1-r2-load-delay/evidence/high-refresh-block-8000293c-20260804-185223`

### Control C — block the shared exception/callback body

The captured OpenBIOS RAM body is `0x80002814..0x800029CC`, with interior
entries `2814, 2818, 2914, 2924, 2934, 293C, 2944, 294C, 2954, 2964`.
Blocking all ten still exits at frame 24003. The final report has
`in_progress=0`, approximately 142.9 million native calls, and approximately
9.1 million `would_run` calls. This contradicts the entire final kernel body as
the root cause; the first semantic divergence occurs earlier.

Evidence:
`lab/sf2/local/generated-disc1-r2-load-delay/evidence/high-refresh-block-kernel-handler-clean-20260804-190851`

An earlier family-control attempt is invalid: concurrent manual debug queries
displaced the route monitor because the server owns one active client socket,
causing a monitor `MemoryError`. Do not use parallel debug clients during the
route.

### Corpus consultation

- `PSX-OVL-004` (additive capture history): confirmed and implemented.
- `PSX-OVL-005` (resident CFG-only capture suppression): contradicted for this
  SF2 set. The title-neutral classifier found zero matches across 65 exact
  variants/52 sources, so no SF3-derived containment was imported.
- The ordinary-Release native ownership-ring diagnostic is applicable and now
  supplies useful atexit evidence.

## Exact resume point

*SUPERSEDED — this seven-step plan was completed on 2026-08-05; see the
"native prerequisite closure" section below. Retained for history.*

1. Re-read the required session documents and confirm the branch/worktree.
2. Do not query port `1994x` from a second client while
   `sf2_mission1_route.py` owns it.
3. Preserve each report in its evidence session before the next run overwrites
   the diagnostic build's `psx_last_run_report.json`.
4. Use `PSX_NATIVE_RANK_LIMIT` to binary-search the ordered current-hash
   candidate set. Each control must run from blank cards and a clean process.
   The semantic result is binary: deterministic frame-24003 exit versus the
   existing state-0 movement gate.
5. At the first rank boundary, identify the exact candidate/CRC and run one
   exact `PSX_NATIVE_BLOCK` control.
6. Compare native and interpreted state immediately before the earliest
   divergent semantic checkpoint. Fix the generic CPU/control/scheduler/cache
   invariant and add a focused regression. Do not ship a blocklist.
7. Only after nonzero compiled-overlay execution passes the route should timing
   ownership be profiled and interpolation implementation begin.

No SF2 runtime or route process was left running at shutdown.

### Prepared first bisection

The rank implementation was inspected after shutdown without launching another
runtime. Ranks are assigned deterministically on each candidate's first live
dispatch, starting at 1; a limit of zero is the all-interpreter endpoint. The
failing clean run observed 443 candidates and the family control observed 449,
so use conservative bounds `[0, 449]` and begin with
`PSX_NATIVE_RANK_LIMIT=224`. The runtime prints one bounded identity line when
each rank is first assigned, including rank, candidate index, owner PC, actual
PC, CRC, DLL, frame, cycle, and allow decision. Preserve that stdout with the
route evidence. Do not infer rank from cache enumeration order.

## 2026-08-05 — native prerequisite closure

The deterministic rank bisection completed with the following clean controls:

- rank 224 pass;
- rank 336 fail at frame 24003;
- rank 280 pass;
- rank 308 fail;
- rank 294, 301, 304 and 305 pass;
- rank 306 fail.

The first changing identity is candidate index 601, rank 306, entry/owner
`0x8001DA48`, code CRC `0x58690F42`, first encountered at frame 24003 and cycle
13549617445. Its shard is `0001C000_7A4D33C6.dll`; the exact range manifest is
`R 8001DA48 5BC`. Full native execution with only `0x8001DA48` blocked passes,
proving a single-owner result rather than a candidate interaction. Rank and
block evidence is retained under the corresponding
`evidence/high-refresh-rank-*` and
`evidence/high-refresh-block-8001da48-20260805-031104` directories.

An exact shadow attempt is invalid diagnostic evidence. It stopped returning
from `func_8001DA48`, had to be bounded externally, and its final report did not
contain an active diff session. Preserve
`evidence/high-refresh-diff-8001da48-20260805-031915` only as evidence that the
shadow tool is unsuitable for this non-bounded candidate, not as a native vs
interpreter comparison.

Mandatory corpus reconsultation pointed back to `PSX-OVL-005`. Read-only byte
comparison found that the 8,196-byte `0x8001C000/7A4D33C6` capture differs from
the configured resident executable at exactly two words:

- `0x8001C754`: resident `0x00000000`, captured data `0x801889A8`;
- `0x8001DA9C`: resident JALR `0x01204009`, captured NOP `0x00000000`.

Only the second word lies in the compiled candidate's exact guarded range. The
existing whole-capture classifier was therefore too broad: unrelated mutable
data outside emitted code concealed the resident control-flow patch. The
generic classifier now evaluates each exact function identity independently
over its range manifest. It marks a shard `unpromoted` when a candidate's
guarded bytes differ from resident text only by inserting/removing control-flow
words. Runtime cache scans omit the marked path on both platforms and the final
load boundary rechecks the marker. No title address or blocklist is embedded.

The exact-range regression covers the SF2-shaped mixed capture, exact resident
text, ordinary in-range mutation, control-to-control mutation, changes outside
the guarded range, multiple identities, marker lifecycle, and loader scan/load
guards. The focused CTest set passes 5/5:

```text
capture_history
crash_overlay_ring_guards
overlay_capture_legacy_history
overlay_dump_bounds
overlay_resident_patch_promotion
```

Two blank-card, clean-process hidden OpenGL routes pass authentic startup and
Mission 1 state-0 movement:

- `evidence/high-refresh-resident-range-gate-20260805-034046`
- `evidence/high-refresh-resident-range-gate-repeat-20260805-035033`

Their comparison reports zero errors, identical semantic fingerprints at all
five checkpoints, identical startup and scheduled-input hashes, identical card
SHA-256 values, and final XYZ `(-5606,2036,7529)`. The repeat has 580 registered
candidates, 145,821,779 compiled-overlay dispatches, 147,947 interpreter
fallbacks, and zero candidate/range-index overflow. Neither complete native
ring contains `0x8001DA48`. The exact rebuilt executable is
`59557D6D1640FFEE236504E5EBC16E5C12AFE1ADA2D1EC37A9834EDDB3E6C0BB`.

A manual classifier invocation accidentally emitted one flavor-1 supplemental
pair into the flavor-0 cache. The loader rejected it by ABI before execution;
it was moved intact to the ignored
`cache/quarantined-wrong-flavor-20260805` directory before the exact repeat.
The active namespace contains no such pair. The ignored footprint is 17.43
GiB, and no owned runtime or route process remains.

## 2026-08-05 — 60 Hz presentation candidate R1

The first isolated presentation candidate uses
`game-high-refresh-pass1-r1.toml` and
`build-high-refresh-pass1-r1/SCUS94451_Recompiled.exe`. It leaves retail
simulation/VBlank timing intact and enables the generic OpenGL interpolator at
60 Hz with VSync off. The config parser's lower bound now matches the runtime:
60 is accepted and 59 is rejected. `sf2_mission1_route.py` records bounded
`gl_interp` state at each semantic checkpoint.

Two clean-card hidden routes passed. In the corrected repeat, TITLE, the
aircraft movie and state 8 report interpolation enabled but suspended,
`history=0`, and `swaps=0`. At player ownership and after movement they report
`suspended=0`, `history=2`, target 60 Hz, and 1,382/1,882 swaps. Five-second
runtime windows stabilize at about 19.9 authentic captures/s and exactly
60.00 presents/s. The player reaches the baseline-identical final XYZ
`(-5606,2036,7529)`, health 150, armor 600, with zero lost CD INT1.

The repeat compares with zero errors against the frozen native route. Startup
and input hashes, all five semantic checkpoints and overlapping guest
fingerprints match. Evidence:

- `evidence/high-refresh-r1-route-20260805-041448`
- `evidence/high-refresh-r1-route-repeat-20260805-042525`

The first comparison exposed a harness-only observation bug: the filtered CD
history endpoint is newest-first, and requesting one record made the claimed
first LEGAL frame depend on host polling. The route now requests 64 matching
entries and selects the oldest `(frame, seq)`; a focused regression protects
this rule. An earlier `041241` launch had `PSX_DEBUG_TOOLS=OFF`, so the route
could not connect and it is not game evidence.

Focused parser/interpolation/route tests and `git diff --check` pass. The exact
candidate executable SHA-256 is
`0199B159814B6E6FD047AC8993FEAFAB3D2C1F7990629A2F95AD63A282C9B851`.
The user handoff is `Launch SF2 High Refresh R1.bat`. Qualification, full suite,
commit and push remain deferred pending visual feedback.

## 2026-08-05 — R1 user rejection

The user reported that R1 felt the same as the accepted build while introducing
motion ghosting. R1 is rejected and will not qualify. Its automated route proof
remains valid only for unchanged retail semantics and measured host cadence; it
did not establish perceptual smoothness or image quality.

The linear shader is a temporal crossfade between complete previous/current
render targets. It has no motion vectors and cannot place geometry at an
intermediate position. The existing motion-adaptive shader merely replaces
the crossfade with a thresholded previous/current choice on strongly changed
pixels. That may suppress trails but cannot supply the missing motion and can
introduce stepping at moving edges, so it is not being handed off as R2.

The source-only 60 Hz parser relaxation was reverted. The ignored R1 launcher
was made non-launching and prints the rejection reason; evidence and the exact
candidate binary remain preserved. Route `gl_interp` telemetry and the fixed
oldest-sector-history selection remain as useful validation improvements.

Any later candidate must interpolate spatial presentation data rather than
blend final images, preserve complete primitive provenance, exclude HUD/FMV/
fades/fullscreen effects, and retain the frozen retail simulation route.

## 2026-08-05 — spatial replay feasibility gate

Read-only comparison with SF-PC-Port confirmed that its shipping spatial path
captures authoritative presentation state and interpolates matched world
transforms; its SF2 guest-packet path itself does not yet provide that result.
The recomp equivalent therefore has to retain and replay retail GP0 geometry,
not borrow title addresses or blend completed framebuffers.

A bounded Mission 1 census proved that bare GP0 source addresses are not stable
identities: SF2 alternates packet heaps on consecutive authentic updates.  The
stable ownership boundary is the dense linked-list submission (consistently
list ordinal 3 in the sampled gameplay), combined with immutable textured
packet material and a conservative mutual-nearest geometry match.  All live
packet/census joins were exact.  Idle gameplay produced 81--87% safe textured
coverage.  During ordinary D-pad movement, the conservative matcher retained
65--70% coverage and rejected ambiguous/untextured packets.  Evidence is under
`evidence/high-refresh-spatial-identity-20260805-045810`; the probe has focused
unit coverage for packet-signature, signed-vertex, reordering, and motion-bound
rules.

The first renderer implementation remains diagnostic-only.  It snapshots the
canonical render target immediately before the dense retail world submission,
records subsequent renderer primitives and their draw state, and reconstructs
an alpha=1 frame in an offscreen FBO.  Pixel equality with authoritative VRAM
is the rejection gate before any spatial interpolation or user-visible output
is enabled.  The rejected R1 executable was copied into its evidence directory
before reusing the isolated build tree.

## 2026-08-05 — spatial replay R2 output candidate

Early alpha=0.5 experiments correctly failed their pixel-change gate even
after packet and screen-coordinate matching. Bounded rejection counters showed
that material/state candidates existed but none passed the geometry bound.
The missing coordinate transform was SF2's alternating PS1 draw page: renderer
positions include the E5 draw offset, so consecutive frames differed by a page
height even when the underlying geometry matched. Matching now subtracts each
record's draw offset, interpolates in page-normalized coordinates, and rebases
the result onto the current retail draw page.

That also corrected the validation target. During sampled gameplay, retail was
scanning out VRAM y=0..239 while drawing the next authentic frame at y=240..479.
Comparing replay against current scanout therefore observed the wrong page.
The corrected draw-page route is
`evidence/high-refresh-spatial-draw-page-20260805-063843`: alpha=1 replay is
pixel-exact for 21/21 samples with zero differing pixels, approximately 305,473
conservative matches include 258,420 moving matches, and alpha=0.5 produces
9,695,239 changed supersampled pixels across the route.

User-visible R2 output queues each eligible spatial half sample until SF2 flips
that same VRAM page to display. The presentation thread chooses discretely
between the spatial half and the authentic current image; it never crossfades
the two, removing R1's source of double-image ghosting. Output requires two
exact alpha=1 replay samples first. If no eligible pair exists, presentation
remains current/authentic. Title, FMV, state 8, fades/fullscreen exclusions,
untextured packets and ambiguous matches all fail closed. In native-wide mode,
the reveal margins remain current/authentic while spatial replay replaces only
the canonical center region.

Two output-enabled routes from clean processes passed:

- `evidence/high-refresh-spatial-output-20260805-065333`: final alpha=1 parity
  21/21 exact, zero diff, 9,695,239 intermediate changed pixels, 589 output
  pairs and 1,777 swaps.
- `evidence/high-refresh-spatial-output-repeat-20260805-070114`: player-owned
  16/16 exact with 428 pairs and 1,287 swaps; final 21/21 exact, zero diff,
  9,695,239 intermediate changed pixels, 592 pairs and 1,787 swaps.

Both routes retain retail route success and report zero spatial frames, output
pairs and swaps during TITLE, the aircraft FMV and state 8. The separately
named handoff executable is
`build-high-refresh-pass1-r1/SCUS94451_HighRefreshSpatialR2.exe`, SHA-256
`BE274DAA80605B452E4991E8CA43C9069B084CFFE9E9ABDEA77DBE328E4ABC97`.
The user shortcut is `Launch SF2 Spatial High Refresh R2.bat` and uses an
independent memory-card directory. Focused spatial identity tests are 7/7 and
`git diff --check` passes. Full qualification remains deferred until the user
accepts motion smoothness and image quality.

## 2026-08-05 — spatial R2 user rejection and rollback

User testing rejected R2 for two independent visible failures: gameplay speed
was unstable and sometimes considerably faster than regular play, and texture
cracking exposed seams throughout the interpolated world. The shortcut is
disabled and the spatial renderer/output implementation has been removed from
active source. The accepted PGXP/native-wide/mouselook baseline remains intact.

The green routes proved only endpoint semantics and authentic-frame replay.
They did not measure guest progression against wall-clock time while the visible
interpolation thread was active, and alpha=1 pixel parity said nothing about
watertight intermediate shared edges. Per-packet mutual-nearest matching can
assign different histories to adjacent triangles; reusing the original UVs then
makes those inconsistencies visible as cracks. This ownership model is retired,
not queued for threshold tuning.

R2 evidence and its exact executable hash remain audit artifacts only. Any next
60 FPS candidate must bind coherent retail object/camera snapshots (as the SF1
architecture does), interpolate shared transforms rather than independently
matched triangle vertices, and add explicit wall-clock progression plus
shared-edge rejection gates before handoff.

## 2026-08-05 — read-only SF1 architecture delta

Read-only inspection of `I:\Projects\sf-pc-port` confirms that SF1 high refresh
does not operate on completed frames or identify GP0 packets after projection.
Its main presentation loop owns an explicit 20 Hz wall-clock accumulator. Each
completed retail update swaps immutable previous/current presentation snapshots;
each host display frame computes `accumulator / 0.05` and rebuilds the native
world from interpolated camera, object, HMD-bone, player and projectile state.
The renderer uses stable semantic object identity (`model`, `class_id`, source
index, destroyed-model state and bone topology). Camera cuts snap, while already
projected guest sprite/line/raw-packet lists stay current rather than being mixed
between ticks. HUD, fades and screen filters remain separate native passes.

The recomp baseline already preserves retail gameplay/hardware timing and has
PGXP provenance, widescreen and direct mouse input. Its missing layer is a
coherent pre-projection presentation snapshot plus a render-at-will path. The
current renderer receives flattened GP0 packets after retail has combined
camera/object transforms and projected vertices. R2 attempted to infer identity
at that late boundary and therefore could not preserve shared topology.

The closest framework-compatible successor is to extend exact PGXP/GTE store
provenance with the RTPS/RTPT input vertex and complete RT/TR/projection state,
publish stable transform groups at the authentic retail world boundary, and
reproject the current retail packet topology from interpolated group transforms.
This must remain a visual side channel: no guest state or retail cadence changes.
Unlike the rejected second-context swap thread, presentation cadence should be
owned by one wall-clock scheduler or otherwise prove that display swaps cannot
advance/perturb guest time. Required gates are independent display FPS and guest
logic FPS meters, one-second guest frame/timer/movement invariance, exact shared-
edge equality after reprojection, and explicit camera-cut/HUD/FMV/fade exclusion.

## 2026-08-05 — transform-snapshot R3 implemented and handed off

The successor described above is implemented as the R3 candidate. RTPS/RTPT
now record the input vertex with complete RT/TR/H/OFX/OFY state and a
transform hash; the world census groups primitives by transform at the dense
linked-list boundary with mutual-best matching and camera-cut snapping; the GL
renderer keeps two page-keyed snapshots (Y=0 / Y=240) with frozen raw-texture
atlases and replays whole primitives from interpolated group transforms.
Capture, output, and parity probes are separately env-gated
(`PSX_GL_TRANSFORM_REPLAY` / `_OUTPUT` / `_PARITY`) and fail closed to the
authentic current frame.

An earlier single-snapshot build produced white flashes and was rejected
before handoff; the corrected two-page build passed the automated Mission 1
route (guest 60.98 Hz, world 18.99 Hz, swaps 60.48 Hz, alpha=1 pixel-exact,
zero seam mismatches). Full detail, including the exact executable hash, is in
the devlog section "2026-08-05 — transform snapshots, flash rejection, and R3
handoff". The user shortcut is
`lab/sf2/local/generated-disc1-r2-load-delay/Launch SF2 Transform High Refresh
R3.bat`. Qualification is blocked on the user's visual verdict.

Post-handoff source corrections recorded 2026-08-06: the world-census begin
call is now gated on the PGXP enable (it previously cleared census state on
every linked-list submission even with the feature off), so qualification must
rebuild from source. Known caveat for the visual test: HUD/FMV exclusion is
implicit (no GTE provenance, no reprojection); geometry submitted in a later
separate linked list would be absent from replayed frames, so HUD completeness
needs explicit visual confirmation. The R3 implementation is intentionally the
only uncommitted work on the branch; the prerequisite classifier/serializer
work and this documentation are committed separately.

## 2026-08-06 — R3 visual rejection, diagnosis, and fixed candidate pending rebuild

The user rejected the first R3 build: a startup black band over the right
fifth of the frame, and a progressive brightening to near-white while
standing still that reset on each authentic frame. No R1-style ghosting and
no R2-style cracking were observed.

Root causes (all confirmed in source, all fixed in the worktree):

1. Present-time pair resolution used the live world census, which every
   linked-list begin resets — replay raced capture and paired old geometry
   with an empty or newer-tick census. Pairs are now frozen into each
   snapshot at the world-submission end boundary
   (`gpu_pgxp_export_transform_pairs`); the live lookup is removed.
2. The alpha=1 parity gate bypassed interpolation (`transform_project_vertex`
   rejects alpha>=1), so the displayed interpolated path was never gated. A
   static-invariance sampler now proves identity-pair captures replay
   pixel-identically at alpha=0.5, and present-time telemetry
   (`pairs_found/pairs_missing/disp_rejects/last_max_disp_px` plus
   invariance counters) is exported in `gl_interp.transform`.
3. The untextured replay path lacked the textured path's depth rejection;
   additive light polygons are now snapped whole on any invalid depth.
4. First-capture bases embed pre-authentic page content (the black band);
   output now requires two captures per page slot.

Validation is NOT done: changes are syntax-verified only. The resume
procedure — package-copy sync, candidate-tree rebuild (never the accepted
directories), telemetry-gated hidden route, then a fresh user handoff — is
in the devlog section "2026-08-06 — R3 user findings, root-cause diagnosis,
and fix implementation". The implicit-HUD-exclusion caveat remains open and
must be checked in the next visual test.

## 2026-08-06 — fixed R3 automated handoff gate passes

The modified runtime files were copied into the generated package source and
only `build-high-refresh-pass1-r1` was rebuilt. A corrupt/truncated Ninja
dependency database caused a persistent `premature end of file` warning and
full rebuilds; it was moved aside as `.ninja_deps.corrupt-20260806`, regenerated
by Ninja, and an immediate build then became a clean two-step no-op. Candidate
executable SHA-256:
`A9D2F393C9301F0D786F04AB63CF16073F1276766EF464681A9FB54646209D10`.

The first smoke attempt is invalid launch-harness evidence only: the command
set an unsupported `PSX_DEBUG_PORT` environment variable, so the healthy
runtime listened on 4370 while the route queried 64137. The corrected fresh-
card run used `--debug-port 64138` and passed the authentic Mission 1 route.
Evidence:
`evidence/high-refresh-transform-r3-frozen-pairs-20260806-110008`.

Automated result:

- invariance 4/4 exact, zero differing pixels;
- parity 2/2 exact, zero differing pixels;
- both snapshot pages valid at Y=0 and Y=240;
- midpoint mismatches, displacement rejects, render failures, and overflows
  all zero;
- cumulative `pairs_missing` unchanged at 255 across the measured player-owned
  through final interval while `pairs_found` advanced 408 to 480;
- guest/world/display cadence 59.99/19.00/60.49 Hz; and
- final retail player XYZ `(-5606,2036,7529)`, health 150, armor 600, with
  zero lost CD INT1.

As with the previous R3 smoke, current-hash overlay shards are absent: resident
AOT executed, while the overlay tier honestly used 13,535,051 interpreter
fallbacks. This is not native overlay coverage and is not newly caused by the
frozen-pair fix. The ignored shortcut `Launch SF2 Transform High Refresh
R3.bat` now points at `build-high-refresh-pass1-r1`; parity readbacks are off
for the user run. The next gate is the user's visual verdict on the startup
band, stationary brightness ramp, and HUD completeness. Stop there before
qualification.

## 2026-08-06 — fixed R3 visually rejected and retired

The user ran the exact rebuilt candidate
(`A9D2F393C9301F0D786F04AB63CF16073F1276766EF464681A9FB54646209D10`)
and reported the same three failures: one-third-rate visible motion,
progressive flashbang, and a black right fifth at startup. The R3 shortcut is
disabled. No qualification, full suite, commit, or push was performed.

The route's own counters explain why its green result was not predictive:
1,876 transform presents produced only 480 successful frozen-pair projection
attempts, and the final snapshot held one pair. Most geometry therefore stayed
at the authentic 20 Hz position. The invariance gate sampled identity-only
snapshots, not the interactive mixed-provenance case. The two-capture gate
proved only a count, not authored margin coverage.

Freezing snapshot pairs closed a genuine live-state race but the unchanged
visual result disproves that race as the dominant flash owner. The remaining
design replays sparse moved geometry and translucent/additive commands over a
pre-world snapshot of persistent wide VRAM that may already contain the prior
world. It cannot reconstruct one coherent intermediate world. This candidate
family is retired; a successor needs complete semantic camera/object/bone
snapshots and a world rebuild, most plausibly supplied by matching decompilation.

## 2026-08-06 — milestone cleanup and playable handoff

The rejected transform source, tests, debug endpoint wiring and route fields
were restored exactly to the branch checkpoint. The three rejected launchers
were removed. Filesystem policy declined recursive deletion of the two
regenerable high-refresh build trees, so they were renamed with a `rejected-`
prefix and left without a launcher; evidence and cards remain as audit data.

The playable shortcut is `Launch SF2 Playable Accepted.bat`. It starts the
accepted PGXP/native-wide executable from
`build-pgxp-pass1-widescreen-r2`, SHA-256
`DE284A5BBBF7C783CC68A90C97937CF8BB9B1AD6B780581178B83E51794C95F2`,
using `game-modern-pass2-build-pgxp-pass1-widescreen-r2.toml` and dedicated
persistent cards under `cards-playable-accepted`. No accepted executable or
configuration was rebuilt.

The milestone is closed. Resume high-refresh work only after a matching decomp
or another complete semantic camera/object/bone snapshot boundary exists.
