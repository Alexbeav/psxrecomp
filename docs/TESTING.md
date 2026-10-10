# Testing

## Running the tests

The tool gate needs Python 3.11 or newer. Four GPU-frame image tests also need
NumPy and Pillow. If either package is absent, those four cases report the
missing package and skip; the other 23 GPU-frame cases still run. This is reduced
image coverage. For the complete gate, install the pinned wheels in an owned
Python environment before configuring CMake:

```powershell
python -m pip install --require-hashes --only-binary=:all: -r tools/tests/requirements.txt
```

The recompiler tree registers the ten tool suites reported in PS1B-476 and
the complete oracle suite from PS1B-375. `tools_psx_chd` uses the shared `chdr`
target from that build. Build the target before running the suite. A build
with `PSXRECOMP_ENABLE_CHD=OFF` omits that entry and does not qualify CHD checks.
The entries use `unittest discover` with an exact file pattern, so a file that
defines test classes without a direct script entry point still runs its tests.
Directly executing such a file can exit successfully without running a case;
the former cursor-writer registration did that with all 29 cases. The entries
reject zero-test results. Suites other than `tools_gpu_frame` also reject skipped
cases. The
`tools_ctest_registration` check reads the generated CTest commands and
properties. A name in a CMake comment does not satisfy it.
It finds CTest at the configured path, then on `PATH`. If neither is available,
that check reports the missing executable and skips with return code 77. An
available CTest that fails remains a test failure. Report this skipped check
and reduced image coverage separately from a complete gate.

The Python `public_commit_guard` test uses isolated local Git repositories and a
local mock SSH transport. It contacts no public remote. It needs Git and Python;
the [public push policy](PUBLIC_PUSH_POLICY.md) describes the separate stage check
and per-clone hook installation.

```sh
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build
cd recompiler/build && ctest --output-on-failure
```

That is the whole thing. CTest reports the enabled and disabled tests for this source.
It needs **no BIOS dump, no disc image, and no generated code**. A plain
recompiler build is enough. Run this check before you open a PR.

On Windows, follow the recipe at the top of
[`WINDOWS_TOOLCHAIN_TRAPS.md`](WINDOWS_TOOLCHAIN_TRAPS.md) first: the toolchain's
`bin` folder first on `PATH` in every shell, a short build directory, and
`psxrecomp-game --help` exiting 0 before you quote a count. Without it a suite
can report dozens of failures that are a DLL load failure and not the code.

The `psxrecomp-game` build writes both codegen-hash headers inside its build
directory. A standalone runtime build writes its header inside the runtime
build directory. Tests verify that neither build writes into the source tree.
The setup packager stages the exact emitter-build header so overlay tools can
still verify that the recompiler matches the runtime cache tag.

Until 2026-07-27 no document in this repository mentioned `ctest`, `pytest`, or
how to run a test at all, so the suite was effectively invisible. If you add a
test, add it to `ctest` in the same commit — an unregistered test cannot fail,
and a test that cannot fail is not a test.

> On a memory-constrained machine, parallel builds of this tree can crash
> `cc1plus` while compiling the toml11-heavy `config_loader.cpp`. If
> `cmake --build` dies with no diagnostic, retry with `-j 2` or `-j 1`; the
> failure is resource exhaustion, not a code error.

### Running one test

```sh
ctest -R overlay_guard_codegen --output-on-failure   # by name (regex)
ctest -N                                             # list without running
```

Every test is also a plain executable or script, so you can run it directly:

```sh
./recompiler/build/bios_address_model_test
python recompiler/tests/test_overlay_guard_codegen.py \
       --recompiler "$(pwd)/recompiler/build/psxrecomp-game.exe"
```

> Pass the recompiler as an **absolute** path. A relative path satisfies the
> test's own `os.path.isfile` check but then fails inside `subprocess.run` on
> Windows with `WinError 2`, which looks like a broken test rather than a bad
> argument.

## What the suite covers

| Group | Where | Needs |
|---|---|---|
| C/C++ unit tests | `recompiler/tests/`, `runtime/tests/` | recompiler build |
| Codegen contract tests | `recompiler/tests/test_*.py` | `psxrecomp-game` |
| Runtime source-invariant guards | `runtime/tests/test_*.py` | nothing — they read source |

The source-invariant guards are the cheapest and most useful class here. They
assert structural properties of the runtime (an ordering holds, a fast path is
invalidated, a fallback exists) by reading the source, so they cost milliseconds
and catch whole regression classes without running a game. Registered from
`recompiler/CMakeLists.txt` rather than `runtime/CMakeLists.txt`, because the
runtime tree cannot configure until a BIOS has been generated, and these need
neither.

## Kernel-table capacity without BIOS data

The recompiler's `kbless_capacity_synthetic` test invokes its built
`psxrecomp-bios` on a wholly authored image. It creates three return functions
in a zero-filled image, its own discovery roots and an isolated profile. The
real emitter must produce a nonempty kernel-body table with the expected
relocated extents and matching row, array and enum counts. No checkout BIOS
profile or seed file supplies that fixture.

The test runs the existing runtime capacity guard on that emitted table. It
checks the actual capacity header, the exact headroom boundary, one row beyond
that boundary and an overflow. Boundary headers are authored controls; their
values derive from the actual emitted count and the guard's headroom policy.
The production capacity and emitter remain unchanged. A missing emitter,
malformed output or failed generation fails the test. It has no skip property.

Unique input/output/provenance directories remain below the configured build's
`kbless-synthetic-evidence` directory. The receipt binds input, output and tool
file hashes. The build and execution records must establish which source
produced that tool; its file hash alone does not prove a source binding.

The runtime's real-profile guard retains its exit77 when no generated BIOS
tables exist. That skip remains unqualified real-profile coverage. The
synthetic test checks the emitter's counting and capacity rules; it cannot prove
that current retail profiles fit. PS1B-308's six methods, fixture generation and
native execution are unrun until admitted validation.

## Disabled performance guards

`recompiler/CMakeLists.txt` registers the performance guards with `DISABLED TRUE`.
CTest lists them and reports them as disabled. This preserves the known failures;
it does not count as a pass. Keep them disabled until an admitted run proves the
checks pass. A source edit or syntax check is not that evidence.

| Test | Status |
|---|---|
| `runtime/tests/test_interpreter_perf_guards.py` | Disabled and unchanged. MMIO synchronization can recompute the device deadline without directly clearing `g_psx_cycle_fast_limit`. The guard also names the legacy `s_next_service_cycle` alias. A timing owner must resolve the invalidation requirement. Source inspection alone does not prove an active production defect. |
| `runtime/tests/test_runtime_perf_diag_guards.py` | Disabled. The retained failure searched for `frame_pacer_wait(&pacer, g_frame_period_ms)`. PS1B-176 changes that name to the current `s_frame_pacer`, preserving the other checks. This source correction has not passed an admitted run. |
| `runtime/tests/test_runtime_perf_diag_guard_controls.py` | Disabled. One current-source positive and twelve deliberate source defects exercise the actual guard. The defects cover opt-in gates, cadence, exact guest-frame boundaries, a one-shot summary, provider timing and counters, and forbidden telemetry in hot paths. All thirteen methods are unrun. |

`overlay_pair_dedup_runtime` is in the normal enabled registration list. Its
companion harness now supplies the step-boundary functions. Its historical link
failure does not make it one of the currently disabled guards.

After execution admission, run the guard and its controls directly through the
qualified runner. The controls also accept `--baseline-guard` with a byte-exact
copy of the guard from accepted commit `79434620340b8b0e347ac8d4e43e93cb3210fad5`.
That baseline against current source must fail, including the current-source
positive. A baseline failure alone does not prove the corrected candidate passes.
The controls copy only these public runtime source files into a temporary fixture;
they do not compile or run the runtime. Their source checks do not prove runtime
timing or performance.

## Tests that are not in `ctest` and should not be

`runtime/tests/` also holds fixtures and harnesses (`*_fixture.c`, `*_harness.c`)
that are inputs to other tests, not tests themselves. Do not register them.

Several C tests under `runtime/tests/` require a **built runtime**, which
requires a generated BIOS (see [`BUILDING.md`](BUILDING.md)). Those are wired
into `runtime/CMakeLists.txt` and run from `runtime/build`:

```sh
cd runtime/build && ctest --output-on-failure
```

## CI

`.github/workflows/cli-release.yml` runs on `workflow_dispatch` and published
releases only. Its header explains why per-PR triggers were removed on
2026-07-25: the Windows job failed often enough that a red check stopped
carrying information, and a check nobody trusts costs attention without buying
confidence.

That reasoning still holds. The gap it left was that no fast, trustworthy
alternative existed. The `ctest` suite above is a candidate: it is hermetic
(no BIOS, no disc, no network), takes under five seconds, and is currently
green. Restoring a per-PR check on top of it is a smaller decision than
restoring the old one.

## Ordinary GPU command queue (PS1B-97/105)

The default renderer now keeps up to 16 pending GP0 words with their DMA
provenance. Device-clock events consume them through the existing parser.
GPUSTAT observes pending work without advancing it. The existing clean
source-GPU cost helpers supply a compatibility schedule; this is not a claim
of measured hardware timing. The source/TAS projection is unchanged.

Default RAM-to-GPU linked-list and block DMA pause when that queue is full.
They read each later RAM word when it is due. A linked-list CHCR stop finishes
the actual current packet before exposing the next header when the consumer
can progress. If active VRAM readback blocks that boundary, stop clears busy
and preserves the unread packet and FIFO so the CPU can finish the readback.
Repeated stops preserve that paused state without advancing time or losing the
unread tail. A matching restart resumes it; a new address or transfer mode abandons that
packet through normal cancellation and starts the requested transfer. No CPU fetch or
data wait was added. The shared device deadline now includes pending GPU work.
Save-state format 15 stores the queue and default block DMA progress and rejects
older versions. GPUREAD latch behavior is unchanged.

Two authored fixtures include the production owners and need no BIOS or game:

```sh
gcc -O2 -flto -fwhole-program -UNDEBUG -DPSX_ENABLE_BLOCK_CYCLES=1 -Iruntime/include runtime/tests/test_gpu_command_queue.c -o gpu_queue_test
gcc -O2 -flto -fwhole-program -UNDEBUG -DPSX_ENABLE_BLOCK_CYCLES=1 -Iruntime/include runtime/tests/test_dma_gpu_command_queue.c runtime/src/dma_gpu_ll.c -o dma_gpu_queue_test
```

Run the first with `pending`, `deferred`, `reset`, `snapshot`, and `packets`.
Run the second with no argument, `upload`, `scheduler`, `stop-read`,
`stop-read-new`, and `stop-read-block`. CMake registers the
same cases for GNU builds. The packet case checks a split quad and a
polyline terminator followed by an attribute. The DMA cases check FIFO pressure,
one-word release at the clock boundary, stop/resume, live later-word mutation,
pending snapshot restoration, and GPU progress without MMIO polling.

The exact-base negative controls use revision
`23a01bd460dd5d9bd80b8fc2767e22d176b0af5f`, with `TEST_BASE` selecting only the
old clock seam. Pending/deferred/reset/snapshot/packets, linked-list pressure,
and upload each fail their behavioral assertion there. Intermediate controls
also reproduce FIFO overflow before DMA gating and premature next-header
exposure before the stop fix. Receipts belong to the task's immutable evidence.

`test_dma_completion_deadline.c` still asserts legacy eager linked-list payload
delivery. That assertion fails on both this candidate and the exact base; it
is an existing adjacent fixture failure, not a passed acceptance gate.

Implementation provenance: only the assigned clean runtime and hardware
behavior documentation were used. No reference-emulator source or preserved
repair implementation was opened. Consulted hardware descriptions:
[GPU status](https://psx-spx.consoledev.net/ps1/gpu/status-register/) and
[GPU ports and FIFO](https://psx-spx.consoledev.net/ps1/gpu/i-o-ports-dma-channels-commands-vram/).
Corpus PSX-BIOS-002, PSX-GPU-001 and PSX-DMA-001 are unreviewed historical leads.
The earlier repair's retail results do not qualify this candidate.

External acceptance still requires the LLE logo, intro speed, RE2 progress,
Spot menu/password text, unchanged Tier 1 seven-route 10k results, and Alex's
visual check. Unit tests do not establish any of those results. In particular,
retaining the parser checks authored ordering but does not prove Spot behavior.
Review must examine the paused DMA stop/resume path, default block timing,
snapshot compatibility, and shared projection
call sites before an integrator builds and runs the retail candidate.

The full-loader fixture `test_gpu_queue_boot_admission.c` includes production
GPU, DMA and boot-state owners, with explicit seams for unrelated devices. It
first admits a valid, fully framed authored state. It then requires invalid
queue count, invalid credit and version 14 to reject without CPU, RAM, GPU,
clock or other device-apply mutations. Both the direct GPU reader and full
loader use `gpu_snapshot_validate` before committing any state. Its GNU command
is the DMA fixture command above with the source file replaced and zlib's
include/library paths supplied (`-lz` on a configured system). CMake registers
`gpu_queue_boot_admission` using `ZLIB::ZLIB`.

Version 15 is a hard input boundary, including source/TAS start states. Before
Tier 1 qualification, the integrator must inventory the actual seven start-state
headers and record their route boundary and source identity. Versions below 15
cannot be used or relabeled as passing. Regenerate compatible starts at the same
defined boundaries and run the same seven routes at 10k, or implement and
validate explicit compatibility. Until that input migration and the unchanged
route gate pass, Tier 1 remains pending. No retail starts were regenerated by
the repair author.
