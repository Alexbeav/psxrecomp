# Current objective — R0 reproducible SF2 project generation

Updated: 2026-08-02

## Objective

Complete feasibility gate R0: generate two clean SF2 Disc 1 projects from the
same pinned inputs, build both, compare their manifests and source trees, and
either demonstrate reproducibility or localize every difference.

After R0 passes, continue directly into the non-visual portion of R1: identify
the resident executable entry/segments and establish a deterministic bounded
boot trace toward CRT and `Game_Main`. Do not wait for user playtesting merely
because R0 completed.

## Verified starting state

- Branch: `experiment/sf2-recomp-feasibility`
- Scaffold commit: `83e0d70`
- PSXRecomp baseline: `0cfa9fe0a8da944e9f694a24361b4973c57131ea`
- CLI package builds successfully.
- Complete framework suite passes 38/38 with `PYTHONUTF8=1`.
- Bundled OpenBIOS exists at `bios/openbios.bin` and is 524,288 bytes.
- SF2 Disc 1 local path is recorded in `.local-context/SF2.md`.
- No retail BIOS path is configured.
- No generated SF2 project exists yet.

## First execution sequence

1. Read all files required by `AGENTS.md`.
2. Verify branch/status/remotes and that sibling repositories are untouched.
3. Verify the CUE and every referenced track exist without printing or copying
   their contents.
4. Hash the CUE and referenced track files. Store hashes only in ignored local
   context until provenance/publication policy is reviewed.
5. First attempt generation with the bundled, redistributable OpenBIOS:

```powershell
$env:PYTHONUTF8 = "1"
$disc1 = "Z:\Emulators\PS1 Games\Syphon Filter 2 (USA) (Disc 1).cue"
$bios = "I:\Projects\SF2-Recomp-Lab\bios\openbios.bin"

.\recompiler\build-cli\psxrecomp.exe build `
  --disc $disc1 `
  --bios $bios `
  --output ".\lab\sf2\local\generated-disc1-a" `
  --name "Syphon Filter 2 Recomp Lab"
```

6. If the CLI requires a distinct retail BIOS backend and rejects OpenBIOS,
   stop that command path cleanly and ask the user only for a local retail BIOS
   path. Do not search broadly for BIOS files.
7. Generate a second clean project at
   `lab/sf2/local/generated-disc1-b` using identical inputs.
8. Build both using their generated build instructions.
9. Compare file lists, sizes, hashes, generated manifests, and build products.
   Normalize only output-root paths and documented host-variant metadata.
10. Create `docs/sf2/devlogs/2026-08-02-recomp-bring-up.md` containing commands,
    tool/input hashes, summarized differences, and the R0 verdict. Do not include
    generated code, disc contents, or large trace excerpts.
11. Add a small reusable comparison script only if manual comparison would be
    error-prone. The script must operate on user-supplied paths and must not
    embed retail data.

## R0 acceptance gate

- Both generations complete from clean output directories.
- Both generated projects build successfully.
- A committed, source-owned comparison method proves reproducibility or lists
  precisely explained nondeterministic fields.
- No proprietary/generated file is tracked by Git.
- Framework tests remain 38/38.
- The devlog records elapsed work, game-specific configuration added, blockers,
  and whether proceeding to R1 is justified.

## R1 initial target

Once R0 passes:

- run the generated project in faithful/LLE mode first;
- keep execution bounded and diagnostics structured;
- identify executable entry, CRT completion, `Game_Main`, and application-loop
  state from independent evidence;
- compare those boundaries with the read-only hybrid oracle;
- quantify native resident, native-overlay, and interpreter dispatch;
- do not begin Mission 3 presentation work before the TITLE boundary is
  deterministic.

## Known environmental detail

Windows is using a Greek legacy code page. Python source-reading tests can fail
with `cp1253` decoding errors unless `$env:PYTHONUTF8 = "1"` is set. This is an
environment issue, not a framework regression.
