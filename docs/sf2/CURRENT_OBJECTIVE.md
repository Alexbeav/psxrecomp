# Current objective — R2 operable retail frontend

Updated: 2026-08-02

## Objective

Advance feasibility gate R2 from the deterministic native TITLE boundary to an
operable retail frontend. Exercise the retail TITLE/MENU/INIT overlay family,
prove cache reuse and invalidation, quantify remaining interpreter fallback by
address range and share, and validate presentation, controller, audio, and
movie-request boundaries without adding SF2-specific substitutes.

## Verified state

- Branch: `experiment/sf2-recomp-feasibility`
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

## R1 verdict

- Passed on 2026-08-02. Retail state 4 is reached with the TITLE shard active,
  native-overlay/cache and interpreter tiers measured separately, and the
  fixed-frame guest/input/presentation boundary reproduced twice.
- Evidence is in `docs/sf2/devlogs/2026-08-02-recomp-bring-up.md`.

## Next execution sequence

1. Repeat the state-8 route with fixed input/state checkpoints and compare
   stable guest state separately from host/query-timing values.
2. Use only retail menu ownership to select the representative Mission 3 route;
   do not substitute the oracle's direct diagnostic bootstrap.

## R2 remaining target

- A second fixed-checkpoint state-8 comparison after the new cache variants are
  present.
- Then select the representative Mission 3 route through retail-owned frontend
  state without modernization or native gameplay substitutes.

## Known environmental detail

Windows is using a Greek legacy code page. Python source-reading tests can fail
with `cp1253` decoding errors unless `$env:PYTHONUTF8 = "1"` is set. This is an
environment issue, not a framework regression.
