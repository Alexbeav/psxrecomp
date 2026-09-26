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

## Batch rule (operator rule, 2026-09-17; Wave 5 batch, PSX Work T59)

Work starts from a base head the operator supplies. Corrections are layered on it; every
route is requalified **once**, at the end.

- Titles are worked one at a time, in the batch order. A title runs only its own route.
  Resume from checkpoints to diagnose; a title is done when one uninterrupted run passes.
- **No reruns of other routes between fixes, and no smoke or prefix runs.** Never rerun an
  already-qualified route because of a fix made for another game.
- Every accuracy fix is its own commit on top of the base, with its `qualified-hunks.json`
  entry. Performance changes are out of scope for a batch.
- Taking in the base, or merging into it, is gated by `build_cache.stage_tools` (every
  registered hunk present) and runs no routes. The base does not move unless the operator
  supplies a new one.
- The batch list is closed; only the operator adds to it. A bug found inside a title gets
  its own PSX Work ID.
- Once every title passes, the whole catalogue is requalified on that one head: every route,
  one uninterrupted run each, at most three at once, with the 120 s watchdog. If a route
  fails, find and fix the cause, then tell the operator before any second full run.

2026-09-17 batch (T59), in order: Abe's Oddysee 5620M, Abe's Exoddus 6672M, Spyro the Dragon
5523M, Spyro: Year of the Dragon 4083M and 5021M, Spyro 2 5278M, MediEvil 4875M.

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
| BizHawk 2.8 | Nymashock (waterbox, `shock.wbx.gz`) | `observer-280-generic` | MediEvil 4875M, Spyro 2 5278M |

**Diagnostic source cores.** When the admitted observer cannot show why a source run did what it
did, build a private instrumented core; never touch the admitted host. `oracle\diag-octoshock-2.7`
holds the 2.7 source with `patch_diag.py` (FrontIO writes/reads, bit clocks, DSR pulses, IRQ edges,
exceptions and DMA steal, with timestamps, gated by `OCTO_DIAG_TRACE="<path>;<first>;<last>"`),
staged in `oracle\BizHawk-2.7-diag`; `crash-01\run-trace.ps1` plays a copy of the movie on it with
a Lua. Its `octoshock.vcxproj` lists `video\convert.h` as a compile unit that races `convert.cpp`
for `convert.obj`; the diagnostic copy drops it. Lua memory callbacks on 2.7 receive no address or
value and cannot see I/O, so they only mark that and when code ran.

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
- **2.8 is the first Nymashock release and differs from 2.9.1+ in the Lua layer.** Its NLua 5.1
  cannot yield under `xpcall`; it reads the Lua file in the ANSI code page and its strings keep
  UTF-16 code units, so the `P1 △` `P1 ○` `P1 □` keys cannot be written as literals (match them by
  their low byte from `joypad.get()`); `memory.hash_region` of MainRAM costs ~317 ms per frame, so
  hash through the helper. Sticks are u16 (neutral 32768), and the helper has no
  `MemoryDomain.EnterExit` to call. `tools/derive_nyma28.py` records each fix with its probe.
  2.8's stock `octoshock.dll` is byte-identical to 2.7's.
- **A 2.8 Nymashock movie needs the 2.8 core.** It desyncs on 2.9.1 (Mega Man X4 6412M), and a
  stick value that is not a multiple of 256 has no 2.9.1 spelling at all (MediEvil 4875M).
- **Multi-disc movies need an `.m3u`** naming both cues. Abe's Exoddus 6672M opens the tray at
  frame 179,993 and closes it on Disc 2 at 179,996; a Disc-1-only host crashed there. That is
  the only swap: its later Disc Select changes all happen with the tray closed, and Octoshock
  mounts a newly selected disc only while the tray is open.

## Build environment

**Launch native builds from PowerShell, not Bash.** Git Bash puts Git's own `mingw64\bin`
first on PATH, whose 2025 libstdc++ cannot load the WinLibs GCC 16 tool binaries. Under ctest
that appears as processes parked at 0 CPU in an `LpcReply` wait with every test timing out at
1500 s, which reads like a hang and is not one. The lane's `.ps1` drivers pin the WinLibs
runtime first and drop Git's copy.

The runtime's starvation watchdog is wall-clock. With several replays and builds running, a host
stall once killed two unrelated multi-hour routes in the same second, so `run_native.py` sets it
to 120 s; the variable is outside the checkpoint identity. Keep concurrent native routes to about
three on this machine anyway.

`setup` refuses a dirty worktree and aborts if HEAD moves during the build. Commit first, then
build, then leave the worktree alone until the route finishes.

A fresh worktree on this machine checks text out with CRLF (`core.autocrlf=true`). Any tool
whose own bytes are hashed into evidence must be pinned `eol=lf` in `.gitattributes`, or
every candidate built from a new worktree is refused ("source reference admission tool
differs"). All five admission tools and `nymashock28_movie.py` are pinned; pin any new one
in the commit that adds it.

## Save states

Read `docs/TAS_CHECKPOINTS.md`. Every title command takes `--save-state-at RETURN...`,
`--save-state-every N` and `--resume-from STATE [--resume-compatible-build]`. Capture during
long qualifying replays, so a later divergence is diagnosed by resuming near it, not by a
cold rerun. Capturing never changes what a run qualifies; a resumed run is always
`diagnostic`, and a final verdict is always an uninterrupted run. Resume with the same
`--returns` as the capture, because the checkpoint binds the route file's content.

States are format v10 and manifests `psx-tas-stateio-v3`, which carry the memory card images
beside the state. A state is bound to its exact executable unless `--resume-compatible-build`
is given, and then only when `PSX_TAS_STATEIO_COMPATIBILITY` matches. Change that identifier
in any commit that changes what a state represents: its layout, and also its meaning with
the layout unchanged. `boot_state_load` checks the header and the section layout, not what a
field means, so a meaning change the identifier does not record lets a resume start silently
from an inconsistent state. T172 was such a change: GPU projection budget units, among
others, changed meaning, so the identifier went from `psx-tas-v10-lane-2` to
`psx-tas-v10-t172` (PS1B-190). A state from before T172 no longer resumes on a T172 build.

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

- **Abe's Oddysee 5620M still fails on one stack page.** Return 949 was the pad DTR session (a
  source lag frame); with that fixed, only page 0x1F0000 differs, on 3,826 of 49,854 returns in
  short bursts from 6,602, with the return clock equal every time and terminal RAM matching.
  Likely below-SP residue from handler or call frames placed at different instructions; open.
- **Tekken 3 still runs on SCPH1001** while its movie declares SCPH-5501. Its comparison uses
  a committed file of line hashes that a BIOS change invalidates.
- **No disc-swap support anywhere in the runtime.** The route format has no tray or disc
  column and there is no playlist concept. Abe's Exoddus needs exactly one tray open (frame
  179,993) and a close on Disc 2 (179,996). Its oracle side is complete.

## Octoshock 2.7 / 2.10 is Nymashock 1.29.0 with a different wrapper and compiler

BizHawk 2.7 moved Octoshock from Mednafen 0.9.38.7 to Mednafen 1.27.1; 2.10 is the same source
plus EXE-only checks. **Mednafen 1.27.1 and 1.29.0 emulate the PS1 identically** (`src/psx` and
`src/cdrom` differ only in messages, VFS and host I/O), so a 2.7/2.10 Octoshock movie runs on the
Nymashock 1.29.0 device switches, not the 2.3 ones. Crash 7798S qualifies that way.

Two places still differ, and both were found by divergence, not by reading:

- **Compiler.** `octoshock.dll` is an MSVC build. CDC `Command_Init` takes
  `std::max(PSX_GetRandU32(0, 3250000), CalcSeekTime(...))`, and MSVC calls `CalcSeekTime` (its
  0-25,000 draw) first where Nymashock's clang takes the broad draw first. Checked in the 2.7 and
  2.10 DLLs' machine code. `--cd-drive-model octoshock-2.7` selects the MSVC order. It is the
  only expression in the 1.27.1 CDC with two draws; check any new source of randomness the same way.
- **BizHawk's wrapper.** The CD shell bit clears only on GetStat
  (`--cd-cold-status-model octoshock-2.2.2`). The DualShock takes stick bytes unscaled and checks
  MODE only when DTR drops; `dualshock_route.read_movie_octoshock27` accepts only records where
  neither can show (every stick 128, MODE never pressed) and refuses the rest.

Both source pads (`InputDevice_Gamepad` at every Octoshock version, `InputDevice_DualShock`)
restart their command phase only on their port's DTR rising edge and ignore a session whose first
byte is not 0x01. A game that probes the memory card and leaves DTR asserted makes the next BIOS
pad read time out in the source: a lag frame. Both `--pad-ack-model` profiles model it.
