# SF2 recompilation feasibility lab — session report

Date: 2026-08-03  
Branch: `experiment/sf2-recomp-feasibility`  
Framework baseline: PSXRecomp `0cfa9fe0a8da944e9f694a24361b4973c57131ea`  
Latest technical commit in this report: `4b5edc7`

## Executive summary

The PSXRecomp-based lab now boots the user-owned US retail Disc 1 through the
frontend and supports a complete user-played Mission 1 route: menus, FMV,
post-FMV intro dialogue, gameplay, real-device audio, `Save and Quit`, card
persistence, and loading the resulting save all worked under retail ownership.
No native replacement was added for gameplay, scripts, AI, collision, camera,
objectives, campaign flow, saves, or authored timing.

This is a strong feasibility result, but it is not yet a claim that the game is
complete or that the comparison protocol has passed R3/R4. The accepted route
has one user validation, not two clean deterministic replays; Mission 3 and the
rest of the campaign remain untested; interpreter fallback is still present;
and the OpenGL backend has a diagnosed 24-bit FMV letterbox-coherency defect.
The software renderer presents the same FMV correctly.

The user reports that this bring-up took about three hours, compared with about
4.5 days for the original process, pending equivalent validation. That is a
promising process improvement, not yet an apples-to-apples performance or
feature comparison: this lab intentionally presents the PS1-compatible profile
at native PS1 display modes, while the other project selected desktop-resolution
presentation immediately. Modern presentation is outside this lab's current
scope.

## Goal and present gate

The experiment asks whether static resident translation, capture-and-compile
overlays, and a bounded interpreter fallback can reach the SF2 retail frontend
and a representative Mission 3 route with less game-specific machinery than
the existing hybrid runtime.

Current assessment:

- R0 reproducible generation: passed.
- R1 resident boot: passed.
- R2 operable frontend/overlay model: functionally demonstrated; deterministic
  two-run checkpoint comparison remains incomplete.
- Mission 1: one complete user-played validation passed.
- R3 representative Mission 3 route: not started.
- R4 architecture decision: premature.

## What worked

- Static resident executable reaches the retail application loop.
- Runtime-installed TITLE/MENU/MOVIE/mission overlays are captured, compiled,
  cached, reused, invalidated, and replaced without editing generated C or
  captures.
- Retail menus accept normal controller input.
- The opening aircraft FMV decodes and its XA audio traverses CD, SPU, and host
  output. Real-device audio was heard in the final validation run.
- Mission 1 crosses the former first-dialogue freeze and remains playable.
- The user played Mission 1 end to end and reported correct behavior for the
  exercised route.
- `Save and Quit` completes through retail memory-card code.
- A newly created save persists to a 128 KiB card image and loads correctly.
- The complete registered framework suite passes 40/40 with `PYTHONUTF8=1`.

Representative post-dialogue native run telemetry at frame 18,191:

- 98,979,844 native-overlay dispatches;
- 313,042 interpreter fallbacks;
- one real overlay invalidation/stale block;
- continued world rendering; and
- zero lost CD INT1 events.

These dispatch counts are route/checkpoint-specific, not percentages for the
entire game. Working fallback is reported as fallback, not native coverage.

## Generic defects found and corrected

### 1. Captured-CFG MIPS-I load-delay violation

An OpenBIOS fasttrack handler used `lw k0,0x4c38(k0); move at,k0`. Captured CFG
emission wrote the load result immediately, so the dependent successor observed
new rather than old `k0` and corrupted the kernel CD queue pointer. The emitter
now preserves the architectural load delay through the immediate successor.

- Lab commit: `663ac4a`
- Upstream extraction: `5834990`
- Upstream submission: [PSXRecomp PR #93](https://github.com/mstan/psxrecomp/pull/93)
- Regression: exact dependent-load sequence

This is generic to any MIPS-I title whose captured control flow relies on a
dependent load-delay instruction.

### 2. Dirty-interpreter non-`$ra` JALR contract

SF2 uses `jalr $a1,$t0` as a descriptor trampoline. The dirty interpreter wrote
`pc+8` through a conventional `$ra` call-unit path even when the encoded link
register was not `$ra`, and rewrote `rd=0` as `$ra`. This fabricated a return
continuation and recursively crossed the dispatcher until the depth guard
fired at Mission 1's first dialogue boundary.

The fix writes the encoded nonzero link register exactly and uses call-unit
optimization only for conventional `rd == $ra`; other JALR forms retain the
faithful flat PC chain.

- Lab commit: `61d3667`
- Regression covers both `rd=0` and a non-`$ra` link

This applies to titles or runtime-installed code that use nonstandard JALR link
registers; it is not SF2-specific.

### 3. Native call-unit IRQ deadlock

`Save and Quit` originally froze before retail issued its first card write.
Card reads, BIOS events, SIO IRQ acknowledgement, card formatting, and callback
execution were healthy. A proposed missing-callback explanation was rejected:
`0x80145360` is an exact compiled candidate and repeatedly executed and returned.

The actual invariant violation was that `overlay_loader_call_native()` raised
`g_call_unit_depth`, while both overlay cycle-interrupt wrappers suppressed all
IRQs at nonzero depth. Retail entered an event pump inside that call and waited
for the next IRQ-backed BIOS event. The call could not return until an IRQ that
the call itself suppressed indefinitely.

Nested call units now deliver IRQs. Existing interrupt-layer machinery still
preserves call-unit atomicity by restoring the interrupted guest thread and
deferring only a requested cooperative cross-thread switch until the clean
outer boundary.

- Lab commit: `dc873fc`
- Registered source regression: `overlay_call_unit_irq_guards`
- Framework suite: 40/40
- Behavioral result: save and load both succeeded
- Retained SIO evidence: 849 closed card transactions, including 120 successful
  retail `0x57` writes and zero transaction aborts

This can affect any title that waits for an IRQ-backed event inside a native
call unit.

## OpenGL 24-bit FMV band defect

The decoded movie is correct, but the OpenGL backend exposes colourful stale
VRAM in the letterbox bands. Bounded live evidence on the affected aircraft
movie showed:

- CRTC scanout: `512x240`, depth 24, display base `(0,0)`;
- horizontal range: `615..3175`;
- vertical range: `16..256`;
- each movie buffer: 32 CPU-to-VRAM strips of `24x160` halfwords;
- displayed buffer coverage: `x=0..767`, `y=40..199`;
- alternate buffer coverage: `x=0..767`, `y=280..439`.

The 160-line payload is therefore correctly centered in a 240-line scanout,
leaving authored 40-line bands above and below. The software-renderer control,
using the identical executable and save, displays those bands correctly in
black. This clean A/B excludes the retail stream, MDEC decode, and general GPU
coordinate calculation.

The code-level lead is OpenGL VRAM coherency:

- OpenGL `gpu_fill()` changes only the authoritative FBO.
- Software `sw_fill_rect()` changes CPU VRAM.
- Depth-24 presentation deliberately reads packed RGB888 from CPU VRAM and
  cannot perform a blanket FBO-to-CPU sync after movie uploads, because that
  would overwrite the packed movie data.
- The movie strips refresh only the central 160 lines, exposing stale CPU-mirror
  contents in the unrefreshed bands.

This diagnosis is strongly supported but the OpenGL correction is not yet
implemented. The next step is to prove the exact pre-movie black fill/copy
sequence with a narrow bounded GP0 transition capture, then maintain CPU/FBO
coherency for the demonstrated operation without hardcoding SF2 dimensions or
blindly blacking all short movies.

The new bounded `depth24_uploads` diagnostic is checkpointed in commit
`4b5edc7`. A deliberately oversized historical GP0 query triggered the debug
server starvation guard and exited that diagnostic process after the affected
frame had already been captured; this was a tooling/query failure, not a new
game failure.

## Known presentation-flow issue

The current automated startup trigger is ambiguous. The probed TITLE word also
equals the trigger value during an earlier 24-bit startup phase; injecting START
there can skip or propagate across presentation. In observed runs, the 989 logo
and legal screen played, while the Eidetic logo and `z_intro` did not.

This has not been attributed to retail or framework behavior. It must be
retested with a compound gate and no pre-title input. The missing presentation
is not counted as a passed boundary.

## Provenance and scope

- No BIOS, disc image, executable, generated game C, overlay capture, RAM dump,
  save state, screenshot, audio, movie, or memory card is committed.
- All proprietary/local evidence remains under ignored lab paths.
- The sibling correctness and presentation projects were not modified.
- No SF2-native gameplay substitute, forced state, skipped retail transition,
  hardcoded callback success, or presentation-only approximation was added.
- The branch remains PolyForm Noncommercial 1.0.0 and should not be described as
  an MIT-compatible or commercially reusable port.

## What remains

Immediate framework work:

1. Capture the narrow GP0 transition into the affected 24-bit movie.
2. Implement and regress the generic OpenGL CPU/FBO coherency correction.
3. Retest the same movie with OpenGL and software backends.
4. Replace the ambiguous startup input trigger and verify Eidetic/legal/
   `z_intro` presentation without early input.

Validation work:

1. Repeat the fixed Mission 1 route from two clean processes with fixed compound
   checkpoints and compare stable guest state.
2. Record dispatch-tier coverage, frame pacing, CPU time, and peak memory at
   comparable checkpoints.
3. Exercise more saves, loads, death/restart, and card-slot/error cases.
4. Test additional missions for overlay convergence and new hardware defects.

Feasibility-plan work:

1. Select Colorado Interstate 70 (Mission 3) entirely through retail menus.
2. Validate opening, player-control handoff, dialogue, combat, objective,
   death/restart, and one checkpoint against the read-only oracle.
3. Measure how native overlay coverage converges and where fallback remains.
4. Produce the R4 architecture/cost report only after those measurements.

Optional upstream work, after focused regressions and user authorization:

- submit the non-`$ra` JALR correction;
- submit the nested call-unit IRQ correction; and
- submit the OpenGL depth-24 coherency correction once implemented and proven.

## Handoff to the other project

The most reusable lessons are:

1. Preserve MIPS-I load-delay semantics across captured CFG block boundaries.
2. Do not assume every JALR uses `$ra`; honor encoded link-register semantics.
3. Define native call-unit atomicity as atomic against cooperative thread
   switches, not against IRQ delivery. Guest code may wait for an interrupt
   inside the call.
4. When a GPU backend has split authoritative representations, audit ownership
   explicitly at 15-bit/24-bit transitions. A correct decode can still expose
   stale mirror data outside the uploaded rectangle.
5. Use bounded rings and semantic checkpoints. Frame-number progress and a
   changing rendered image are not proof of crossing the same retail boundary.

## Primary repository records

- `docs/sf2/CURRENT_OBJECTIVE.md` — current verified state and next sequence.
- `docs/sf2/devlogs/2026-08-02-recomp-bring-up.md` — chronological commands,
  evidence, failed hypotheses, and accepted conclusions.
- `docs/sf2/FEASIBILITY_PLAN.md` — R0–R4 gates and stop conditions.
- `docs/sf2/COMPARISON_PROTOCOL.md` — deterministic comparison requirements.

## Overnight addendum — deterministic closure

The previously open OpenGL FMV, startup-flow, and two-run Mission 1 gates are
now closed:

- generic depth-24 CPU/FBO ownership fix and title-neutral regression;
- generic CD SeekL/SeekP active-read cancellation and regression;
- complete authentic 989/Eidetic/legal/ZINTRO/TITLE startup with no input;
- compound stable TITLE gate before exact guest-frame input;
- two clean native-enabled retail Mission 1 routes to verified player movement;
- exact matching same-frame RAM/PC/MMIO/scratchpad/cycle fingerprints at all
  semantic checkpoints;
- separate resident AOT, compiled overlay, and interpreter fallback counts;
- complete framework suite 43/43 and 6.604 GiB ignored footprint.

Interpreter fallback is approximately 0.480% of overlay-tier dispatches in the
final pair and remains required, visible, and excluded from native coverage.
The next feasibility target is retail-selected Mission 3. A short visible
OpenGL acceptance remains useful tomorrow but is not needed to establish the
completed headless semantic route.

Detailed records:

- `OVERNIGHT_REPORT_2026-08-03.md`
- `PSX_PORTS_RETURN_2026-08-03.md`
