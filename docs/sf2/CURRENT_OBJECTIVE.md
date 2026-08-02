# Current objective — R1 deterministic TITLE boundary

Updated: 2026-08-02

## Objective

Continue feasibility gate R1 from the now-proven resident bootstrap to a
deterministic TITLE boundary. Localize why runtime-loaded resident code is
currently interpreter-owned, establish the correct overlay capture/native-cache
route without editing generated code, and report native resident, native
overlay, and interpreter dispatch separately.

## Verified state

- Branch: `experiment/sf2-recomp-feasibility`
- Scaffold commit: `83e0d70`
- PSXRecomp baseline: `0cfa9fe0a8da944e9f694a24361b4973c57131ea`
- R0 passes: two corrected-package generations contain 992 identical
  non-build files after exact output-root normalization and both build.
- Final PE products differ only in documented PE/build-ID timestamps and the
  derived checksum; normalized product hashes match.
- CLI packaging now includes the Vulkan shader embedder required by generated
  projects, with regression coverage.
- Complete framework suite passes 38/38 with `PYTHONUTF8=1`.
- Bundled OpenBIOS LLE identity and loaded checksum match.
- Two clean headless runs reproduce the same boot call chain:
  `0x800F8598` at frame 727, `0x80029624` at frame 727, and
  `0x80029700` at frame 728.
- Frames 1–8, 593–600, and 727–730 have identical structured fingerprints
  across clean processes.
- Dispatch misses are zero through the measured bootstrap.
- TITLE is not proven. Near frame 755, native overlay dispatch is zero and
  interpreter fallback is about 2.95 million; at frame 1,806 it exceeds
  41 million with no registered overlay region.
- No retail BIOS path is configured; OpenBIOS is sufficient for the measured
  resident handoff.

## Next execution sequence

1. Reproduce the headless LLE bootstrap with the three boot candidates armed
   from process initialization.
2. Use bounded overlay status/candidate/capture rings to identify the first
   resident overlay write/load boundary after the application loop.
3. Determine whether the lack of overlay registration is configuration,
   capture discovery, invalidation, or loader behavior. Produce a manifest or
   reproducible proof before changing code.
4. Fix only a generic framework defect if evidence supports one. Never patch
   generated C or substitute an SF2-native frontend.
5. Run twice from clean processes and compare fixed-frame fingerprints plus
   stable guest/application state.
6. Do not claim TITLE until state 4 ownership, the TITLE overlay, and stable
   presentation/input boundaries are observed together.
7. Record native resident, native-overlay, and interpreter counts separately.

## R0 verdict

- Passed on 2026-08-02. See
  `docs/sf2/devlogs/2026-08-02-recomp-bring-up.md`.

## R1 remaining target

- Preserve faithful/LLE mode and structured bounded diagnostics.
- Treat the deterministic entry/`Game_Main`/application-loop chain as the
  resident starting boundary, not as proof of TITLE.
- Reach and reproduce retail application state 4 with the TITLE overlay active.
- Quantify native resident, native-overlay, and interpreter dispatch.
- Do not begin Mission 3 presentation work before the TITLE boundary is
  deterministic.

## Known environmental detail

Windows is using a Greek legacy code page. Python source-reading tests can fail
with `cp1253` decoding errors unless `$env:PYTHONUTF8 = "1"` is set. This is an
environment issue, not a framework regression.
