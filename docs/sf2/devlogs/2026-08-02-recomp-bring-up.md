# 2026-08-02 — SF2 recompilation bring-up

## Scope

Complete R0 reproducible project generation, then begin the non-visual R1
resident-executable boot gate with bounded headless instrumentation. No runtime
window was opened. All retail-derived and generated outputs remained under
ignored local paths.

## Starting state

- Branch: `experiment/sf2-recomp-feasibility`
- Framework baseline: `0cfa9fe0a8da944e9f694a24361b4973c57131ea`
- Initial worktree: clean
- Remote: fetch-only `upstream`; push URL disabled
- Bundled BIOS: `bios/openbios.bin`, 524,288 bytes
- Initial ignored experiment footprint: 824 bytes

The Disc 1 CUE and its one referenced track both existed. Their SHA-256 values
are recorded only in `.local-context/SF2.md` pending provenance/publication
review. The extracted `SCUS_944.51` independently matched the committed
reference SHA-256
`75a360bf7465dfdec85c14f9ba93862aae2531b48d83fd8d82ba8c9fffa13d33`.

## R0 generation and packaging repair

The first documented invocation used
`recompiler/build-cli/psxrecomp.exe`. It failed while copying the framework:
the build-tree executable treated the repository root as its payload, and an
output nested beneath `lab/` recursively collided with that source tree. The
312,019,058-byte partial output was preserved under the ignored directory
`failed-build-tree-generation-20260802` because direct deletion was disallowed.

`python tools\build_cli.py release` produced the supported versioned package at
`dist\psxrecomp-cli-windows-x86_64`. Its `psxrecomp.exe` SHA-256 was
`8422fb632ce30f537b41d6c19ee607ca95c8b82623dbb4c710ec1dfc509858fc`.
Generation from that package succeeded, but the first generated-project build
localized a framework packaging defect:

```text
ninja: error: .../psxrecomp/tools/embed_spirv.py ... missing
```

`runtime.cmake` requires this source-owned Vulkan shader tool, while
`tools/build_cli.py` copied only `runtime/` and `recompiler/`. The package
producer now copies `tools/embed_spirv.py`, and `cli_project_packaging` guards
that dependency. Generated outputs were not patched; both were quarantined and
regenerated from the corrected package.

## R0 evidence

Two clean generations completed from identical inputs:

- A: 2,681 ms
- B: 2,464 ms
- executable entry: `0x800F8598`
- load range: `0x80010000` through `0x801EC800`
- initial stack: `0x801FFFF0`
- game translation summary: 3,625 functions and 95,063 basic blocks
- OpenBIOS translation summary: 650 functions emitted, four skipped

Both generated projects built successfully with 578 Ninja actions:

- A: 117,321 ms
- B: 113,295 ms

The reusable comparison command was:

```powershell
$env:PYTHONUTF8 = "1"
python tools\compare_generated_projects.py `
  lab\sf2\local\generated-disc1-a `
  lab\sf2\local\generated-disc1-b `
  --exclude-top-level build-r1
```

All 992 non-build files matched after normalizing only the exact absolute A/B
project roots in UTF-8 text. The normalized tree SHA-256 was
`d3a0b1cadf63ab663ace34e8afae79e9144aa744026c6f2a228f544f613bea4a`.
There were no missing or differing generated, input, manifest/config, seed,
BIOS, or framework-source files.

The final PE products had the same 42,369,131-byte size but different raw
hashes. A byte-level comparison localized the complete difference to six bytes:
the PE timestamp, its derived PE checksum, and two digits in the explicit
`__DATE__/__TIME__` runtime build ID. This command normalizes only those fields:

```powershell
python tools\compare_generated_projects.py `
  lab\sf2\local\generated-disc1-a `
  lab\sf2\local\generated-disc1-b `
  --pe-product build/Syphon_Filter_2_Recomp_Lab_Recompiled.exe
```

Both products then had normalized SHA-256
`ab2b7f81b8012cf6c4040fb328960c8f8c099c312f265d1680ce931a720290d2`.

The pinned framework suite passed 38/38 after the packaging fix. R0 passes:
generation is reproducible, both outputs build, the only product differences
are explicitly identified host/build metadata, and no proprietary/generated
file is tracked.

## R1 resident boot

A separate `build-r1` tree was configured with `PSX_DEBUG_TOOLS=ON`; the two R0
products were left unchanged. Runs used `--headless --no-launcher`, unique TCP
ports, and fresh ignored memory-card directories. The active BIOS reported:

- image `OPENBIOS`, SHA-256
  `fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57`;
- loaded checksum matched the configured image;
- backend `LLE (recompiled BIOS)`;
- boot skip and HLE call routing both disabled.

The first eight frame fingerprints matched across two clean processes. Frames
593–600 also matched across clean processes while OpenBIOS was still reading
the disc. Candidate addresses came from the read-only hybrid oracle and were
armed from process initialization with:

```powershell
$env:PSX_FNTRACE_ARM = '0x800F8598,0x80029624,0x80029700'
```

Two clean runs produced the same bounded call records and the same frame
727–730 fingerprints:

| Frame | Target | Caller RA | SP | Boundary |
|---:|---:|---:|---:|---|
| 727 | `0x800F8598` | `0xBFC06694` | `0x801FFFF0` | PS-X entry |
| 727 | `0x80029624` | `0x800F863C` | `0x807FFFF8` | `Game_Main` candidate |
| 728 | `0x80029700` | `0x800296E8` | `0x807FFFE0` | application loop candidate |

The call chain agrees with the independently generated entry and the oracle's
retail call map. A live post-entry read of `0x8011EE8C..0x8011EE9B` was identical
in both runs: depth 1, current state 0, transition 0, and the following word 0.
Dispatch misses remained zero.

This proves deterministic resident boot through the first application-loop
boundary. It does not prove TITLE. Around frame 755, native overlay dispatch
was still zero while interpreter fallback was about 2.95 million dispatches.
At frame 1,806 the fallback count exceeded 41 million and covered runtime-loaded
addresses in the resident-overlay window; no overlay region was registered.
This is the next coverage/blocker investigation, not native recompilation.

## Tooling observations and rejected inferences

- The TCP server closes the I/O-thread ping connection; probes must use the
  documented one-command-per-connection client behavior.
- `pause` is intentionally removed and returns an explanatory error. Evidence
  therefore comes from bounded rings and fixed frame ranges, not a synthesized
  live snapshot.
- `dispatch_check` uses a finite recent ring and returned false for the early
  entry after long runs. The boot-armed function trace is the valid proof.
- `quit` returned `emu busy or frozen` while still causing the owned headless
  process to exit successfully. No user-owned process was terminated.
- Two forced-stop/debug interactions produced automatic bounded freeze JSON
  artifacts. They were not used as evidence and were moved with all per-run
  memory cards to ignored `lab/sf2/local/r1-runtime-artifacts/`.
- High aggregate static-hit counts before frame 727 were BIOS execution, not
  evidence that SF2 had started.

## Footprint and next step

The final ignored experiment footprint was 1.748 GiB, including recoverable
failed-package quarantines and the R1 instrumentation build. The next action is
to localize why runtime-loaded resident code remains entirely interpreter-owned,
then establish a deterministic TITLE boundary with native-overlay/cache and
interpreter shares reported separately.
