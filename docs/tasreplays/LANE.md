# TAS replay lane — read this first

One entry point for the lane. Everything here is a fact a tool cannot derive for itself:
external host behaviour, machine layout, and standing debts. Anything a check *can* enforce
lives in a check, not here — see the gate below.

## The gate

`tools/tasreplays/qualified-hunks.json` records, per accuracy change, the exact lines a
qualifying replay depended on. `build_cache.stage_tools` verifies them, so **no title
adapter can build a candidate from a tree that silently dropped one.** Register a behaviour
in the same commit that qualifies it (CLAUDE.md rule 19). A known loss goes in the
registry's `known_absent` map with a written reason and is reported as a debt.

```
python tools/tasreplays/qualified_hunks.py check --rev HEAD      # one tree
python tools/tasreplays/qualified_hunks.py check-all             # across the split branches
```

## Where things live

Lane root is **`D:\psxrecomp\validation\tas`** (operator rule, 2026-09-16: keep lane files on
D:). Under it: `oracle/` pinned BizHawk hosts and rebuilt observer cores, `tools/` launchers,
helpers and admitted source references, `movies/`, `firmware/`, `chd/` harness-split dumps,
`discs/`, `runs/` source passes and their PowerShell drivers, `native/` candidates and routes,
`build-cache/`. Older `nb-*` evidence stays on C: where admitted references bind it.

Evidence archives go to `Z:\Share\psxrecomp\tas-evidence\<name>\` — `archive_run.py` for a
native route, `archive_source_runs.py` for source passes.

## Oracle hosts, and what each one costs you

| Host | Core | Helper | Notes |
|---|---|---|---|
| BizHawk 2.2.2 / 2.3 | Octoshock | `Observation230` | Tekken 3, Pepsiman, Abe's Oddysee |
| BizHawk 2.9.1 | Nymashock (waterbox) | `Observation291` | Bio Hazard, Mega Man X5, Resident Evil DC |
| BizHawk 2.10 | Nymashock | `observer-2100-generic` | Mega Man X4 6790M |
| BizHawk 2.7 / 2.10 | Octoshock | `ObservationOcto2x` | Crash 7798S, Abe's Exoddus 6672M |

**One helper per core family per host.** A 2.10 Nymashock helper does nothing for 2.10
Octoshock. Stock Octoshock throws from `TotalExecutedCycles` at every version, so the observer
role always runs a rebuilt core carrying the `b3ec859c` passive cycle export; it is admissible
only through the stock passivity comparison.

Host quirks, each found the hard way:

- **Only one EmuHawk at a time.** The source harness verifies a file closure over a shared
  install directory. Native builds and routes may run alongside a source pass; other source
  passes may not.
- **2.10 starts paused.** Its Lua runs after the frame loop has advanced, so the frame-zero
  assertion fails unless the host config sets `StartPaused`. The unchanged Lua unpauses.
- **2.7 and 2.10 dropped `BizHawk.Client.Common.Global`.** The read-only, movie-end, sync and
  settings introspection the 2.3 Lua did through it now lives in the managed helper.
- **2.7 needs its exact `MainVersion`** (`2.7.0`, not `2.7`) in the written config's
  `LastWrittenFrom`, or it stalls on a hidden "Mismatched version in config file" prompt.
- **On 2.7/2.10 the movie-end action lives under `Movies`**, and 2.10 defaults it to Pause.
- **Display method must be GDI+** on this machine; Direct3D device creation fails on a modal.
- **EmuHawk cannot read the `\\172.16.1.8` UNC firmware path.** Keep a local BIOS copy.
- **The dialog watcher must decline a save prompt.** A fatal-exception dialog offers to save
  the movie; answering Yes would write to the frozen original.
- **Observer storage**: the page log is ~8.7 KB per frame, so the launcher budget is 6 GiB.
  Exoddus at 482,352 frames needs ~4.2 GB.
- **Multi-disc movies need an `.m3u`** naming both cues. Abe's Exoddus 6672M opens the tray at
  frame 179,993 and swaps discs twelve times; a Disc-1-only host crashed at the first swap.

## Build environment

**Launch native builds from PowerShell, not Bash.** Git Bash puts Git's own `mingw64\bin`
first on PATH, whose 2025 libstdc++ cannot load the WinLibs GCC 16 tool binaries. Under ctest
that appears as processes parked at 0 CPU in an `LpcReply` wait with every test timing out at
1500 s, which reads like a hang and is not one. The lane's `.ps1` drivers pin the WinLibs
runtime first and drop Git's copy.

`setup` refuses a dirty worktree and aborts if HEAD moves during the build. Commit first, then
build, then leave the worktree alone until the route finishes.

## Adapters: derive constants, never clone them

Every adapter is a near-clone of another, and every cloned constant that was never exercised
has eventually been wrong: entry point and text size, executable name, disc serial, controller
hash, step cap, random-tape length. Cross-check pinned constants against the artifact they
describe — `abesoddysee.py` and `megamanx4.py` verify the game profile against the boot
program's own PS-X EXE header, which turns a silent wrong build into an immediate refusal.

Two finite resources that have bitten: the Nymashock cold random tape (65,536 words by
default; Mega Man X4 draws about one per return and exhausted it at return 67,863, so it now
runs on a 1,048,576-word tape) and the route step cap, which is one constant shared by the
runtime header, the encoder and `run_native.route_identity`.

## Standing debts

- **`155003cb` save-state capture and resume is absent from this lineage.** Dropped in the
  PR #361 → split rebase like `b1413e71`, and unlike it never restored. Costs replay time
  only, never accuracy: all runs are cold. Recorded in `known_absent`; port it from
  `pegasus-codex/biohazard-resume-20260913` and delete the entry.
- **Tekken 3 still runs on SCPH1001** while its movie declares SCPH-5501. Its comparison uses
  a committed file of line hashes that a BIOS change invalidates.
- **No disc-swap support anywhere in the runtime.** The route format has no tray or disc
  column and there is no playlist concept, so Abe's Exoddus cannot be replayed natively until
  that is designed. Its oracle side is complete.
- **No runtime source-behaviour model for Octoshock 2.7 or 2.10.** The runtime implements
  2.2.2, 2.3 and Nymashock 1.29.0 only, so Crash and Exoddus need those cores characterised or
  proven equivalent before a native route means anything.
