# Current objective — modernization pass 1 acceptance

Updated: 2026-08-03

## Objective

Preserve compatibility checkpoint `2009297` on
`experiment/sf2-recomp-feasibility` while validating the explicitly authorized,
isolated first high-resolution and keyboard/mouse pass on
`experiment/sf2-modernization-pass1`. The modernization branch may change only
generic presentation and host-input translation. Retail gameplay remains the
owner, and the baseline executable/cards remain available for A/B diagnosis.

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
