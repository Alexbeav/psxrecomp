# Tekken 3 TAS replay

Build and play [Spikestuff's tool-assisted speedrun](https://tasvideos.org/4164M)
from your own Tekken 3 USA disc and SCPH1001 BIOS. The original 7,974 inputs
finish Arcade with Yoshimitsu at **8.80 seconds**. This branch reproduces the
qualified Octoshock 2.2.2 timing configuration in the native psxrecomp runtime.

## Quick start: Windows x64

Install **Git, Python 3.11 or newer, CMake 3.24 or newer, Ninja, and a UCRT
MinGW GCC toolchain**. Put their executables on PATH. The validated compiler is
WinLibs GCC 16.1.0, x86_64 UCRT POSIX SEH. MSYS2 users should use the UCRT64
environment, not MINGW64/MSVCRT. Python uses only its standard library.
Allow several GB of disk space and a few minutes for the initial build.
Internet access is needed for the original TAS and the pinned SDL3/zlib sources.

```powershell
git clone --branch tasreplays https://github.com/Alexbeav/psxrecomp.git
cd psxrecomp
python tools/tasreplays/tekken3.py setup --disc "D:/Games/Tekken 3 (USA).cue" --bios "D:/BIOS/SCPH1001.BIN"
python tools/tasreplays/tekken3.py run
```

The setup verifies all three original disc tracks and the BIOS before building.
It copies the verified media into the ignored `build/tekken3/media` directory,
extracts the boot executable, downloads the TAS, preserves every input, builds
the tools, runs their tests, generates BIOS/game C, checks all 57 generated
source fingerprints, and builds the player. No emulator, reference oracle,
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

Use `--timeout 3600` on a slower machine, or `setup --jobs 4` to reduce peak
compiler memory. An original `.bk2` or its TASVideos download ZIP can be supplied
with `setup --movie "D:/TAS/spikestuffv3-tekken3-ps1.bk2"`; it must have the
same identity as publication4164. Setup can be repeated in the same checkout;
moving a configured checkout requires rerunning setup to resolve its new paths.
The native executable imports only Windows system libraries; the launcher does
not need the compiler's DLL directory at playback time.

## Exact inputs

The supported disc is the original three-track USA BIN/CUE layout from the
[TASVideos version record](https://tasvideos.org/Games/1530/Versions/View/1913).
Renaming files is fine when the CUE names them correctly. Track order, raw
sectors and audio pregaps must match. Merged BIN, ISO, CHD, other regions and
revisions are not admitted by this setup.

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
guard after return10,704. That later behavior is still unresolved. This branch
does not claim unrestricted post-victory playback, complete audio/pixel parity,
save-state support, PAL support, or other-game TAS success.

The source starts from the exact validated framework base
`f23c5ba1a220fe1ca8818cc48c026d6c2f7f2c64`; it is a dedicated branch in the
Alexbeav fork. It does not update that fork's main branch or upstream master.

## Tests without retail assets

```powershell
cmake -S recompiler -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build/tests --parallel 8
ctest --test-dir build/tests --output-on-failure
```

The suite includes the existing recompiler tests, the ABI22 overlay boundary
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
