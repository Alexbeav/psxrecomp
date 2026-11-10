# Tekken 3 TAS replay

For explicit, bounded multi-step investigations and read-only evidence analysis,
see the [unattended investigation controller guide](investigation.md).

Build and play [Spikestuff's tool-assisted speedrun](https://tasvideos.org/4164M)
from your own Tekken 3 USA disc and SCPH1001 BIOS. The original 7,974 inputs
finish Arcade with Yoshimitsu at **8.80 seconds**. The original campaign
qualified the Octoshock 2.2.2 comparison configuration. This upstream integration
is pending fresh retail qualification; see [current status](../../docs/TAS_UPSTREAM_INTEGRATION.md).
Setup retains the historical generated-code fingerprint gate and can refuse
changed output from the updated upstream emitter.

## Quick start: Windows x64

Install **Git, Python 3.11 or newer, CMake 3.24 or newer, Ninja, and a UCRT
MinGW GCC toolchain**. Put their executables on PATH, with the compiler's `bin`
folder first, in PowerShell and in Git Bash alike: see the recipe in
[Windows toolchain traps](../../docs/WINDOWS_TOOLCHAIN_TRAPS.md). The validated compiler is
WinLibs GCC 16.1.0, x86_64 UCRT POSIX SEH. MSYS2 users should use the UCRT64
environment, not MINGW64/MSVCRT. Python uses only its standard library.
Allow several GB of disk space and a few minutes for the initial build.
Internet access is needed for the original TAS and the pinned SDL3/zlib sources.

```powershell
git clone --branch codex/tas-upstream-integration-20260913 https://github.com/Alexbeav/psxrecomp.git
cd psxrecomp
python tools/tasreplays/tekken3.py setup --disc "D:/Games/Tekken 3 (USA).cue" --bios "D:/BIOS/SCPH1001.BIN"
python tools/tasreplays/tekken3.py run
```

The setup verifies all three original disc tracks and the BIOS before building.
It binds the original CUE and tracks in place, caches the verified boot executable
and BIOS, downloads the TAS, preserves every input, builds the tools, runs their
tests, generates BIOS/game C, checks all 57 generated source fingerprints, and
builds the player. When the build cache reuses the tools, setup runs no tool test: it prints
`Tools test gate DID NOT RUN in this setup` with the test count of the cached run, and
`Tools test gate ran in this setup` with the count when it ran them.
BIOS emitter freshness checking stays enabled. No emulator, reference oracle,
Ghidra, prebuilt game binary, generated game C, or files from the research
workspace are required. No retail disc, firmware, or extracted game code is
distributed in the branch.

The first run opens a window and ends automatically at frame 8,400, after the
victory is visible. It checks **8,399 completed frame returns**, including every
original-input return, against the saved RAM-page and cycle fingerprints.
Success prints `PASS`; a changed input, missing checkpoint, different RAM page,
or clock difference produces a nonzero exit and identifies the first mismatch.
Each run gets a new directory under `build/tasreplays-runs`, containing logs,
screenshots, input completion, RAM/clock records and `verification.json`.

For unattended verification:

```powershell
python tools/tasreplays/tekken3.py run --headless
```

To fast-forward a visible replay, use `run --speed 4`, `--speed 8`,
`--speed 32`, or `--speed max`. Supported caps are 1, 2, 4, 8, 16, 32 and 64;
`max` removes the wall-clock cap. These use the same host pacing and
presentation path as holding the fast-forward key. Every original input and
RAM/clock checkpoint is still verified. The cap is a ceiling: CPU, rendering
and capture costs can keep the achieved rate below it. Headless playback is
already uncapped, so combining `--headless` with a speed override is rejected.

The standalone runtime accepts `PSX_FAST_FORWARD=1` to hold fast-forward from
startup and `PSX_FAST_FORWARD_SPEED=2..64|max` to choose its cap. The replay
command sets these explicitly and records them in each run's manifest.
After updating an older checkout, run `setup` with a fresh `--project` directory
and pass that same directory to `run` before selecting a speed.
The runner rejects older setup receipts so a player cannot silently ignore
the requested fast-forward control.

Use `--timeout 3600` on a slower machine, or `setup --jobs 4` to reduce peak
compiler memory. An original `.bk2` or its TASVideos download ZIP can be supplied
with `setup --movie "D:/TAS/spikestuffv3-tekken3-ps1.bk2"`; it must have the
same identity as publication4164. Each setup requires a fresh output directory.
Use `setup --project "D:/psx-tas/tekken3-build-02"` to preserve a previous build,
then use `run --project "D:/psx-tas/tekken3-build-02"`. An explicit project directory
must be outside the source checkout. `--cache` selects an existing verified
private boot/firmware cache, and `--tools-dir` selects a reusable tools build
that setup reconfigures and tests. Moving a configured checkout requires a new
setup so generated paths and firmware provenance are verified again.
The native executable imports only Windows system libraries; the launcher does
not need the compiler's DLL directory at playback time.

## Exact inputs

The supported disc is the original three-track USA BIN/CUE layout from the
[TASVideos version record](https://tasvideos.org/Games/1530/Versions/View/1913).
Renaming files is fine when the CUE names them correctly. Track order, raw
sectors and audio pregaps must match. Merged BIN, ISO, other regions and
revisions are not admitted by this setup.

A `.chd` of that same layout is also accepted for `--disc`: setup splits it
with `chdman extractcd -sb` into one BIN per track plus a CUE, in a private
directory keyed by the container's own SHA-256, and then runs the identical
size/hash/layout check over the result. The check is not relaxed for a
container, so a CHD of a merged, ISO or wrong dump fails exactly as the
equivalent CUE does. `chdman` is taken from `--chdman`, else `PSX_CHDMAN`, else
`PATH`; `--chd-cache` overrides where the split tracks are kept.

| Input | SHA-1 |
|---|---|
| Data track, 632,532,768 bytes | `68f32a8657376cb4d4504c160fd5e8ae4a4f18d6` |
| Audio track2, 27,701,856 bytes | `ba8cbc6a371250a92b3d12c1154f489bd6b4b70b` |
| Audio track3, 28,042,896 bytes | `a5b34e39603ed80fed8400d879b20b39aadfe1ba` |
| SCPH1001 BIOS, 524,288 bytes | `110155d8d6e6e832d6ea66db9bc098321fb5e8ebf` |

The script checks the full SHA-256 values as well. The BK2 SHA-256 is
`13ebc56bb877ed3200ca3dcd54dac25ad20cba091a73351425ec8bbabc82e5d6`.
Its original header requests SCPH5501; the qualified native and independent
reference runs both used the SCPH1001 image listed here. Do not substitute the
5501 image into this particular recipe.

## Scope and evidence

This is an explicit source-emulator compatibility profile, not a claim of
complete PS1 hardware timing or general game compatibility. It starts from
cold boot, uses one digital controller, no second controller, no memory cards,
software rendering, LLE BIOS and the qualified instruction/DMA/device timing
options. It does not shift inputs, patch game RAM, load a checkpoint, replace
game code with the reference core, or select delays from the desired outcome.
The profile's raw CD random tape is generated independently of the movie and
game state by the separately licensed external utility described below.

The reference fingerprints come from integrated202, the second verified native
victory. At all 7,974 original-input returns, every one of its 512 RAM-page
hashes and its return clock matched the independent Octoshock reference.
Twenty-eight selected full 2 MiB RAM snapshots also matched byte for byte.
The neutral tail fingerprints through8,399 come from that native victory run.
The small committed reference file contains hashes only, not guest RAM.
See [accuracy changes](../../docs/TAS_ACCURACY.md) for the repair scope.

The replay deliberately ends at the observed victory boundary, matching the
declared426 neutral inputs after the original movie. A longer exploratory tail
in the research build stopped at the explicit **unqualified CDDA Play seek**
guard after return10,704. That statement describes the original Tekken revision; later optional CDDA
component work is documented in [the source CDDA profile](../../docs/source-cdda-profile.md). This branch
does not claim unrestricted post-victory playback, complete audio/pixel parity,
save-state support, PAL support, or other-game TAS success.

The historical campaign started from `f23c5ba1`. This submission starts from
upstream `85cd26f0`. It does not change release pins or merge itself.

## Diagnostic runs: prefixes, ladders, streaming stop and binary identity

The private source-drive Pause correction distinguishes standby after a completed
SeekL/SeekP from an already-paused drive, even though their public status bits
are equal. Standby uses the existing settle formula at the moving physical head and its
existing random draw; a repeated Pause remains fast and draws nothing. The
logical SeekL header pipeline is backed out before settling even without an
active data read. Original CD countdown/RNG observations pin that distinction.
This private source-profile candidate includes marked reference-derived code.
default drive path and checkpoint encoding are unchanged. Vagrant6315 original
RAM captures establish the first frontier at return3016; focused controls alone
do not establish a full native replay pass.

The three title `run` commands (`tekken3.py`, `pepsiman.py`, `biohazard.py`)
share these flags. None of them changes the verdict: the existing post-run
comparison still decides a pass exactly as before.

- `--returns N` replays the first N original inputs plus one record with no
  neutral tail and compares N returns (`N` below the input count); `N` equal to
  the endpoint is the full run. The shortened route is written beside the run
  as `<output>-input.psxrti` / `.psxrti2`. Receipts carry `diagnostic_prefix:
  true` and `status: "prefix_pass"`; a prefix never claims the full pass.
- `--ladder 6000,60000,full` runs strictly increasing prefix tiers into
  `<output>-t<N>` and the optional final `full` tier into `<output>` itself,
  stops at the first tier that does not match, and writes
  `<output>-ladder.json`. Mutually exclusive with `--returns`.
- `--stop-on-divergence` lets the streaming watcher (`stream_compare.py`),
  which follows `ram-pages.tsv` while the runtime writes it, publish
  `<run>/stop-request.json`; `run_native.py` then terminates playback with
  `stop_reason: "harness_stop"` and records the request in `exit.json`. The
  run is a failure like any divergence. The watcher result is always recorded
  under `streaming` in the receipt (`verification.json` for Tekken 3 and
  Pepsiman, `source-comparison.json` for Bio Hazard).
- `--exe PATH` runs another executable than the setup receipt's; it must be
  declared with `--diagnostic-binary SHA256` naming the bytes that actually run,
  otherwise the launch is refused. Receipts record `setup_executable_sha256`,
  `binary_sha256`, `binary_matches_setup` and `diagnostic_binary`; a mismatching
  binary yields `status: "diagnostic"` and a nonzero exit, never a qualifying
  pass. `run_native.py` binds the staged copy with `--expected-exe-sha256` and
  records `staged_executable` in `manifest.json`. The private run copy keeps
  the source executable's permission mode, so Unix executables stay executable.
- `--cpu-boundary-window LO HI` is forwarded unchanged to `run_native.py`.
- `diverge_trace.py <run-a> <run-b> <return> --output <dir>` is a diagnostic, not a
  qualification. It takes the return clocks at R-1 and R from both `cpu-return.tsv`
  files, forms the window [cycle at R-1, cycle at R + `--margin-cycles`] clamped to
  the 1,000,000-cycle runtime limit, resolves each side's binary by SHA-256 (the
  staged copy in the run directory, then `manifest.inputs.exe.path`, then any
  `<evidence-root>/*/SHA256SUMS.txt` entry that still hashes correctly), reruns
  both sides through the title harness of run-b's project with `--returns`,
  `--diagnostic-binary` and `--cpu-boundary-window`, and streams the two
  `cpu-boundary.tsv` traces row by row ignoring the cumulative `slice_takes` and
  `slice_cycle` counters. It prints the identical-row count and the first differing
  row with every differing column, and writes `<output>/diverge-trace.json`.
  `--dry-run` prints the window and commands; `--compare-only` diffs two
  directories that already hold traces. Exit 0 reports a difference, 2 identical
  traces, 1 an error.

## Qualified-hunk registry

`qualified-hunks.json` records, for every accuracy change a replay pass depends
on, the file and the exact added lines as they exist in the qualified tree
(`227e9057`). `python tools/tasreplays/qualified_hunks.py check --rev <rev>`
verifies each block is still present contiguously at that revision (CRLF and
trailing-whitespace insensitive, indentation exact); `check-all` prints an
entry-by-revision matrix over `origin/split/01..07` and `HEAD` (pass
`--revs upstream/master,...` after a fetch). This is an integrity check on
source trees, not a replay verdict: presence proves nothing about behaviour,
absence is a definite finding. It is how the dropped `b1413e71`
`dirty_ram_interp.c` hunk of 2026-09-14 would have been caught before a full
replay. New fixes are added with `seed --commit SHA --id ID --titles ...
--as-of 227e9057`; blocks a later commit rewrote are split into the sub-runs
that survive and the entry is marked `evolved`.

## Archiving a run

`python tools/tasreplays/archive_run.py <run-dir> <archive-name> [--evidence-root DIR]
[--mailbox DIR] [--dry-run] [--label TEXT]` copies a finished run directory into
`<evidence-root>/<archive-name>/runs/<run>/` (default root
`Z:\Share\psxrecomp\tas-evidence`, or `PSX_TAS_EVIDENCE`), verifies every copied
file by SHA-256 and byte total, copies the sibling ladder/prefix receipts and the
project's `setup.json`/`game.toml`/`bios.toml`/`input.json` into `runs/` and
`builds/<project>/`, and appends the new files to the archive's `SHA256SUMS.txt`
without ever rewriting an existing hash (a conflicting hash fails before anything
is copied; an identical existing copy is reported as already archived). It then
writes `runs/<run>.archive.json` and, under `notes/`, a mailbox note
(`YYYYMMDD-HHMM-<AUTHOR>-<title>-<run>.md`, dropped into `--mailbox DIR` with its
`.md.sha256` sidecar only when that flag is given) and a one-paragraph board
amendment that is also printed. The summary restates the run's own receipt
(`source-comparison.json` or `verification.json`, the `streaming` block, the
binary identity fields, ladder and setup receipts when present): it archives
evidence and drafts notes; it qualifies nothing. `--dry-run` prints the summary,
the planned paths and both texts without writing.

## Setup build cache

Every title `setup` reuses its three build stages through a content-keyed cache
(design: `docs/tasreplays/build-cache.md`, code: `build_cache.py`). Stage 1 is
the tools build plus ctest, stage 2 the generated BIOS/game C (and census
outputs), stage 3 the native player. A stage is reused only when every input
that can influence it is identical by content hash (git tree ids of the
committed source trees, SHA-256 of ROM/boot/seeds/generated files and tool
executables, the toolchain banner lines and the CMake flags); paths, mtimes and
branch names are never inputs. Reuse is a shortcut for building only: media,
movie, reference and firmware hashes, the BIOS emitter fingerprint and the
Tekken codegen guard still run on every setup, and `run` never consults the
cache. A partial or corrupt entry is a miss, never an error.

Flags shared by `tekken3.py`, `pepsiman.py` and `biohazard.py setup`:

- `--build-cache DIR` selects the cache; otherwise `PSX_TAS_BUILD_CACHE`,
  otherwise `%LOCALAPPDATA%\psxrecomp\build-cache`.
- `--no-build-cache` builds every stage fresh and stores nothing.
- `--tools-dir DIR` keeps its meaning (reconfigure, rebuild and retest there)
  and bypasses the tools stage of the cache.

On a miss the stage runs exactly the commands it always ran; the tools build
lands in `<cache>/tools/<key>/build/` instead of `<project>/tools`. On a hit the
entry's files are copied into place and re-hashed, and the stage logs
(`configure-tools.log`, `generate-bios.log`, `build-native.log`, ...) are copied
into the project with a first line naming the entry they came from. A native hit
copies only the executable into `<project>/native/`; no CMake build directory is
created. `setup.json` records every stage:

```
"build_cache": {
  "root": "<dir>",
  "tools":     {"key": "<hex>", "hit": true,  "entry": "<path>", "built_at": "<iso>", "source_head": "<sha>"},
  "generated": {"key": "<hex>", "hit": false, "entry": "<path>", "built_at": "<iso>", "source_head": "<sha>"},
  "native":    {"key": "<hex>", "hit": false, "entry": "<path>", "built_at": "<iso>", "source_head": "<sha>"}
}
```

`source_head` names the commit whose setup produced the entry; a stage that was
not keyed (`--no-build-cache`, or `--tools-dir` for the tools stage) has
`key: null`. `executable_sha256` and every other field keep their meaning.
`python tools/tasreplays/build_cache.py list` shows the entries;
`build_cache.py prune --keep-days N` deletes older ones. Nothing in setup deletes.

The player used to embed `<project>/game.toml` as its default game-config path
(`DEFAULT_GAME_CONFIG_PATH` in `tools/tasreplays/CMakeLists.txt`), the only
absolute path in the binary and the reason two byte-identical builds in
different project directories never shared a hash. It is now the relative
`game.toml`, resolved beside the executable; the harness is unaffected because
`run_native.py` always passes `--game`. Binaries built after this change do not
reproduce the 2026-09-14 hashes, which were project-directory specific anyway.

## Private full-input checkpoint candidate

For an original core with no CPU-register observer, the private runner accepts
`--stock-reference <original-directory> --stock-ram-only`. The directory must
contain the actual `pages.tsv`, an `original-registers-unavailable.json` receipt,
and no `cpu-actual.tsv`. This compares all 512 RAM pages at each completed return;
it does not compare or infer source CPU registers or absolute cycles. Omitting
`--stock-ram-only` preserves the required CPU-and-RAM comparison. A RAM-only match
is not full CPU qualification. `--continue-on-divergence` records the first RAM
difference while the unchanged movie, input audit and checkpoint saves continue.

The PS1B-109 private candidate adds checkpoint continuation for a full-audit
DualShock route. This source change is not yet a qualified full replay.
`--save-state-at` saves the existing guest state and writable card images, plus
`<state>.input-observer`. The manifest binds that sidecar's SHA-256 and the
mounted disc. The sidecar carries the actual applied-input hash contexts,
boundary journal, console-event receipt prefix and route/session cursors.
It does not reconstruct past observations from the movie.

`--resume-from` with `--full-input-audit` requires that complete checkpoint,
the same executable by default, route, configuration, media and card topology.
Explicit `--resume-compatible-build` permits a rebuilt executable while retaining
the intact actual input journal and all runtime state-compatibility/ABI, RAM,
cycle, BIOS, entry, configuration, route and card checks. The run manifest names
both saved and running executable hashes. This does not establish correctness of
the rebuilt runtime: compare its actual resumed interval against the oracle.
In the source SPU profile, this explicit flag also permits a legacy native SPK2
checkpoint to migrate to SPK3. SPK2 has no reverb filter histories; migration
starts those new histories at zero and logs that fact. It preserves the saved
native voices, queues and CD state. This is a version migration, not an exact
reconstruction of missing histories: only an oracle comparison of the resumed
interval establishes that a particular saved checkpoint is viable. Short or
malformed footers remain rejected before machine state changes.
A restored input that was
already applied before its CPU return must not be applied again. MODE latch
state and the two card images remain part of the existing guest/card state.
Legacy non-audit manifests remain readable.

For a legacy native checkpoint whose clock-tape digest still contains its old
host path, pass `--resume-legacy-clock-path` with that exact recorded path and
`--resume-legacy-clock-sha256` with its recorded tape SHA-256, together with
`--resume-from` and `--resume-compatible-build`. The launcher requires both
options and checks the current tape SHA before launch. It records their runtime
variables `PSX_TAS_RESUME_LEGACY_CLOCK_PATH` and
`PSX_TAS_RESUME_LEGACY_CLOCK_SHA256` in the run manifest. Inherited `PSX_*`
variables remain stripped; direct runtime invocation can set those two variables.
The current
`PSX_CD_SOURCE_CLOCK_TAPE` must match those bytes. This reconstructs only the old
path value for the legacy digest; other model values, route, media and card
checks remain exact. The native CD snapshot also checks its own saved tape
fingerprint/count before committing state. Missing files, changed bytes and
unrelated configuration changes remain rejected. New saves use content identity.

`--terminal-return-probe` observes the last original CPU/RAM return with zero
neutral tail. On resume, CPU/RAM comparison covers only the new return interval;
the earlier interval needs its retained comparison record. A complete input
hash alone does not prove guest-state or gameplay equivalence. Native save/load
and later-disc continuation require actual uninterrupted/resumed controls.

`--continue-on-divergence` records the first oracle mismatch without requesting
a playback stop. Input delivery, checkpoint saving and the full tape continue;
the mismatch still makes the comparison fail. Without this option, the existing
first-divergence stop remains the default. A crash or a completed tape is recorded
separately from the oracle-comparison verdict.

## Tests without retail assets

```powershell
cmake -S recompiler -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build/tests --parallel 8
ctest --test-dir build/tests --output-on-failure
```

The suite includes the existing recompiler tests, the ABI24 overlay boundary
tests, O0/O2 TAS input/clock/controller regressions, BK2 validation, and failures
for incorrect media topology and replay evidence. Three pre-existing disabled
tests remain visible in CTest. Historical fixtures that need the separately
retained original-core oracle captures remain explicitly listed in
`runtime/check_test_registration.cmake`; they are not counted as portable CI
passes. CI contains no BIOS, disc, generated title C or retail replay.

## Licenses and attribution

The framework and new independent runtime code retain the repository license.
`external/source_random_tape.py` is a **separate GPL-2.0-or-later program**,
adapted from the fixed cold-reset random generator in Mednafen/Octoshock.
Its [source and license notice](external/README.md) and complete GPL text are
included. It is invoked as its own process and exchanges only a binary data
file with the native runtime. It is not compiled or linked into the runtime.
The GPL notice applies to that utility, not as a relicensing of this repository.

The TAS is credited to Spikestuff and is downloaded unchanged from the
publication page. It is not bundled with the repository.

Private checkpoint format16 preserves all16 memory-control words, RAM_SIZE and CACHE_CTRL. Older checkpoints omitted these observable registers and are rejected; absent values are not inferred from BIOS defaults or oracle registers. Full native requalification must start cold when its retained checkpoint lacks this bank. The existing full-machine atomicity fixture covers the write-save-change-load-read failure seen in FFVII.

The private original1.29 GPU table gives copy/upload/download readiness feedback after two queued header words, independently of full packet lengths4/3/3. Vagrant26088 saved a ready bit during the block-copy header interval; native previously used packet length as the feedback threshold. The scalar FIFO fixture fails before and passes after the readiness correction. Packet consumption, work-cost formulas and renderer are unchanged. Guest polls can follow the source-correct branch; no clock offset or retiming is applied. Full title qualification remains a separate run.

PSXRTI2 full-input auditing uses the same TAS_SOURCE_CONTROLLER_V1 frame-stamped hash as PSXRTI3, including the selected axis conversion and any declared neutral P2. A missing expected-hash field in older PSXRTI2 run manifests can be recalculated from the hash-verified retained route and checked against every retained applied-input row without executing the game. Preserve the original failed validator receipt alongside that data-only correction.

The private RAM-only oracle reader also accepts `pages-segments.json` in a stock reference directory. Its explicit segments name local files, SHA-256, inclusive return bounds and either frame/pages or the existing frame/clock/pages layout (gzip is supported). This consumes the retained observations directly: it never synthesizes frame zero, CPU registers or a clock comparison. Complete selected coverage, ordinal continuity, hashes and all512 page fields are checked. FFIX can bind its already-verified original prefix and continuation; VHD can bind its original return1..endpoint raw capture. This is a capture-format adapter, not an assertion that different source providers are equivalent. The existing streaming-comparison fixture covers valid segments, missing/overlapping returns, changed bytes and a reference starting after zero.

The explicit older-provider --spu-keyoff-model octoshock-1.11.9 and narrowly checked native transition are described in [TAS checkpoints](../../docs/TAS_CHECKPOINTS.md#versioned-spu-key-off-behavior).

For a compatible native checkpoint whose original multi-disc game configuration
contains paths from another host, pass --resume-relocate-discs together with
--resume-from, --resume-compatible-build, and every --roster-disc in route
order. Keep the original game TOML byte-identical. This explicit option writes a
bounded versioned mapping in the run folder. The runtime checks the original
configuration, route, all original roster positions, mapped boot disc, and each
mapped image digest before checkpoint admission. Existing route content/order
validation also runs. Inherited environment settings never enable relocation.
Normal starts and resumes without this option keep their existing path checks.
