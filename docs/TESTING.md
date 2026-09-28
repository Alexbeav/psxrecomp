# Testing

## Running the tests

```sh
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build
cd recompiler/build && ctest --output-on-failure
```

That is the whole thing. CTest reports the enabled and disabled tests for this source.
It needs **no BIOS dump, no disc image, and no generated code**. A plain
recompiler build is enough. Run this check before you open a PR.

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

## Known-failing tests (not registered)

Three tests exist and are **deliberately left out of `ctest`** because they fail
today. They are not registered because a suite with a known-red test is a suite
people stop believing — the exact failure mode that took CI off pull requests in
the first place (see `.github/workflows/cli-release.yml`). Fix or retire them,
then wire them in.

| Test | Status |
|---|---|
| `runtime/tests/test_interpreter_perf_guards.py` | Asserts `psx_devices_mmio_sync` invalidates the inline cycle limit. It does not: the function delegates to `psx_devices_service_to_now()` (which clears `g_psx_cycle_fast_limit`, `psx_cycles.c:161`) **or** to `psx_devices_recompute_deadline()` (`:153-157`), and that second branch never clears it. Needs a timing owner to decide whether the guard found a real hole or the invariant moved. The guard is also partly stale — it still names `s_next_service_cycle`, since renamed to `psx_next_service_cycle`. |
| `runtime/tests/test_runtime_perf_diag_guards.py` | Asserts a substring that is no longer present in the runtime source. Either the diagnostic was removed or it was renamed; the guard has not been updated either way. |
| `runtime/tests/test_overlay_pair_dedup_runtime.py` | Needs its companion harness (`overlay_pair_dedup_harness.c`) built. Unlike the other Python tests it is not source-only, so it needs a build target before it can be registered. |

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
