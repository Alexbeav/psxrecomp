# Setup fallback reports

`generate` and `rebuild` report fallback decisions in their result JSON.
Each `degrades` row has a stable `code` and a plain-language `reason`.
A `degrade` progress event names the decision when it occurs.
The existing success, error and default build policies still decide the exit code.
An empty fallback list does not prove that the game is playable or that a package is qualified.

The commands save separate reports under the project's `.cache` folder.
`degrade_report` names the saved file, its state and its UTC recording time.
`complete` means that reporting reached the command's final result.
`incomplete` means that the command did not reach that result.
`not_saved` means that persistence failed; `report.persistence` also appears in CLI output.
An older saved report can survive such a failure.
These are last-recorded command reports, not a test of the current installation.

`--setup-selfcheck` adds `setup_degrades`, with `scope: "last_recorded"` and separate
`generate` and `rebuild` objects. Each has `state`, `recorded_at` and `degrades`.
States are `recorded_complete`, `recorded_incomplete`, `missing`, `unreadable` or `invalid`.
Missing or invalid records do not mean that no fallback happened.
The reader performs no build, subprocess, cleanup or file write.
The readiness exit codes remain 0 for complete sources, 2 when setup would reopen,
and 1 when the host cannot identify the project.

| Code | Reported decision |
|---|---|
| `toolchain.system_fallback` | The portable toolchain was unavailable; system CMake was used. |
| `bios.openbios_regen` | OpenBIOS generation failed; an already staged retail backend was retained. |
| `bios.emitter_stamp` | An optional emitter fingerprint could not be written. |
| `mtime.clamp` | Timestamp inspection or repair missed one or more paths. |
| `lto.low_memory` | The automatic macOS memory policy disabled link-time optimization. |
| `lto.unsupported` | The compiler could not support requested link-time optimization. |
| `pgo.unavailable` | Profile-guided optimization lacked its merge tool. |
| `pgo.failed` | Profile-guided optimization failed; the plain product was built. |
| `overlay.staging` | Overlay compiler staging failed for the named product folder. |
| `overlay.tcc_tier` | Staging succeeded with tcc, a small compiler, but without an optimizing compiler. |
| `diagnostic.optional_failure` | The additional diagnostic product was unavailable. |
| `notices.partial_or_missing` | Licence copying was incomplete or carried toolchain texts were missing. |
| `cleanup.incomplete` | Requested cleanup could not remove one or more paths. |
| `disc.track_list` | The track list was not checked or an advisory mismatch or kit fault was tolerated. |
| `report.persistence` | The fallback report could not be saved. |
| `report.truncated` | The bounded report exceeded 128 rows; it is partial. |

Licence counts remain counts of copied files. They do not establish complete staging or
licence suitability. Normal and diagnostic staging reports include their product folders.
Explicit `--no-lto` and `--no-pgo` choices are not failed optimization attempts.
Protected symlink targets and active build folders remain excluded from timestamp repair.
Fatal disc, BIOS, emitter, main-build and failed plain-fallback errors remain fatal.
Diagnostic-only failure stays fatal; an optional diagnostic failure keeps the normal product.
See [Diagnostic mode](DIAGNOSTIC_MODE.md).
Shell selection keeps the shared policy in [Windows toolchain traps](WINDOWS_TOOLCHAIN_TRAPS.md).
