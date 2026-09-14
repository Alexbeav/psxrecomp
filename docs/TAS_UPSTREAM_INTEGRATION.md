# TAS upstream integration status

This draft ports the Tekken 3, Pepsiman and Biohazard replay work onto upstream
master `85cd26f0`. It is a source submission, not a qualification of the new
combined runtime. Release pins and existing campaign builds remain unchanged.

## Review map

| Area | Entry points |
|---|---|
| Profile and historical Tekken evidence | [TAS accuracy](TAS_ACCURACY.md), [replay tools](../tools/tasreplays/README.md) |
| CPU, exception and overlay integration | `recompiler/src`, `runtime/src/traps.c`, `runtime/include/overlay_api.h` |
| GPU and DMA service | `runtime/src/source_gpu_runtime.c`, `runtime/include/source_gpu_service_clock.h`, `runtime/src/dma.c`, `runtime/src/gpu.c` |
| CD-ROM, MDEC, SIO, SPU and timers | Corresponding modules under `runtime/src` and focused fixtures under `runtime/tests` |
| Biohazard source profile and limitations | [Nymashock source replay](NYMASHOCK_SOURCE_REPLAY.md) |
| Replay investigation and input identity | [Investigation guide](../tools/tasreplays/investigation.md) |
| Asset-free checks | `tools/tasreplays/tests/CMakeLists.txt`, `recompiler/CMakeLists.txt`, `.github/workflows/tasreplays.yml` |

The foundation was published in fork PRs Alexbeav/psxrecomp#28, #29 and #30,
but had not reached upstream master. This branch starts from upstream and
selectively ports that foundation, subsequent Pepsiman fixes through
`39484acb`, controller/Biohazard work through `b73c16f3`, and cold-run,
investigation and GPU corrections through `7a0d0ff4`. It also ports the ordinary
scheduler precise-owner and pending GPU return corrections from `b1413e71`.

Integration preserves upstream asynchronous GPU linked-list DMA, CFG metadata,
UNC handling and widescreen/mod callbacks. Overlay ABI 24 appends TAS callbacks
to upstream ABI 23. The generated-code identity checks remain enforced; old
fingerprints do not qualify a freshly generated upstream build.

The bounded GPU upload-history fix is independently submitted as
[upstream PR #360](https://github.com/RetroPortingToolKit/psxrecomp/pull/360).
This integration contains the same fix and its regression test.

## Evidence boundaries

| Historical source build | Result | Limit |
|---|---|---|
| Tekken `0ff6e765` | 8,399 return clocks and RAM-page hashes matched; the original 7,974 inputs plus 426 neutral inputs reached YOU WIN, FINAL 8.80. | This is the original campaign build, not this rebased integration. Speed settings preserved identity but measured about 0.45x throughput. |
| Controller `fd4edc81` / equivalent Biohazard cherry-pick `85cf6437` | Cold Tekken 8,399/8,399 and Pepsiman 75,406/75,406 matched; Pepsiman terminal RAM matched. | The first Pepsiman attempt hit its watchdog at 60,863. An unchanged executable/input/profile retry on another local volume passed; the host cause remains unknown. |
| Biohazard `7a0d0ff4` | 239,202 returns and clean exit; exact prefix through 233,567; persisted card matched. | First clock mismatch at 233,568 was one cycle early; terminal RAM differed by nine bytes. This is not a fidelity pass. The historical native tail also differs from stock by one neutral input. |

The controller acceptance record is the 2026-09-13 corpus snapshot with SHA-256
`dab598ca4673896c86daa8dceb06181637c9d47642c171840fc1bfde8d0d1904`.
Private replay traces and machine-local evidence are not part of this submission.
The compressed Tekken reference contains comparison hashes, not RAM contents.
The large GPU JSON fixtures are synthetic command/oracle cases.

## Validation on this integration

The recompiler and asset-free TAS targets build with GCC 16.1 on Windows UCRT,
CMake/Ninja, and Python 3.12. All 160 enabled CTest tests pass; the three
pre-existing disabled tests are `interpreter_perf_guards`,
`runtime_perf_diag_guards` and `overlay_pair_dedup_runtime`.
All 28 changed Markdown files pass the relative-link check.
Focused scheduler and pending-GPU-return fixtures execute production modules
at both `-O0` and `-O2`; unrelated link seams abort if reached.

The actual `psx-runtime` target also links with the no-BIOS configuration,
debug tools enabled, and UI, netplay, rewind, setup wizard and Vulkan disabled.
This build check does not exercise a retail game. No new retail replay has
qualified this combined upstream base.

## Deferred work

The T52 boot-state wire/checkpoint stack and the newer `155003cb` checkpoint
implementation are excluded. Their dependencies and full/cross-build restore
qualification are separate work. This branch exposes no checkpoint launcher
and makes no arbitrary-resume or cross-build restore claim. Source-profile CD
snapshots remain rejected when the required state cannot be represented.

Before this draft is marked ready, regenerate and qualify the integrated retail
builds with their exact input, source and output identities. Resolve or explicitly
retain the Biohazard mismatch as a documented profile limitation. Do not infer
release acceptance from the historical results or asset-free tests.
