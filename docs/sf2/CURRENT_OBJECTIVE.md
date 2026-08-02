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

## R1 verdict

- Passed on 2026-08-02. Retail state 4 is reached with the TITLE shard active,
  native-overlay/cache and interpreter tiers measured separately, and the
  fixed-frame guest/input/presentation boundary reproduced twice.
- Evidence is in `docs/sf2/devlogs/2026-08-02-recomp-bring-up.md`.

## Next execution sequence

1. Establish deterministic controller injection at the state-4 TITLE boundary
   and drive only retail transitions into MENU/INIT.
2. Capture and compile any newly executed frontend overlays, then repeat from a
   clean process to prove cache reuse.
3. Exercise a real overlay replacement and record invalidation/revalidation
   evidence; do not infer it from initial cache loads.
4. Attribute the remaining 17,416 interpreter fallbacks by address range and
   execution share, separating unseen code from unsupported code.
5. Record both display pages, controller state, SPU/XA activity, and STR
   requests with bounded diagnostics.
6. Repeat milestone comparisons twice from clean processes and distinguish
   stable guest state from transient host/query-timing status bits.

## R2 remaining target

- An operable retail frontend reached through retail-owned input and state
  transitions.
- Demonstrated overlay capture, compilation, reuse, and invalidation across the
  TITLE/MENU/INIT family.
- Measured native resident, native overlay, and interpreter shares by relevant
  address range.
- Verified two-page GPU presentation, controller input, SPU/XA activity, and
  STR requests without modernization or native gameplay substitutes.

## Known environmental detail

Windows is using a Greek legacy code page. Python source-reading tests can fail
with `cp1253` decoding errors unless `$env:PYTHONUTF8 = "1"` is set. This is an
environment issue, not a framework regression.
