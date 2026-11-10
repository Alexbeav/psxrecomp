# Isolated upstream consumer checks

These development tools exercise the existing private Ape consumer without
moving the stage or a replay pin. They are not final fleet or console-oracle
qualification. Use the normal host resource check and one serial driver.

`tools/upstream_consumer_verify.py` uses the existing Ninja commands and title
objects to prepare reference/native host-pacing, image-sharing and snapshot
variants. Only their selected runtime objects are recompiled. The original
executable and Ninja log are hashed before and after each run. Image/snapshot
variants add identical read-only witnesses to both arms in temporary source
copies. Their timing includes those witnesses and cannot establish throughput.

Run `noop` before the payload. `prepare --family 789`, `784` or `788` retains
the compiler/linker arguments and identities. Then use `run --family NUMBER
--policy reference|native --sample NUMBER` and `summarize --family NUMBER`.
Each run has a fresh output folder; existing evidence is never overwritten.

Runs use a never-shown window, dummy audio and scripted player input. Visible
owned windows or a timeout stop that owned process tree. A changed desktop
foreground is recorded, not automatically attributed to the consumer.
Presentation checks require `--frames 1800` to reach the exercised path; these
use a separate 16:9 runtime configuration. The initial 600-frame boot prefix
did not cover shared-image publication and remains explicitly incomplete.
Rewind uses supported depth 50 and interval 4, so 600 frames saturate the ring
and exercise buffer reuse. Snapshot allocation counts cover the writer only.

`tools/upstream_pgo_verify.ps1` drives the maintained
`tools/runtime_hot_pgo.py` baseline/generate/use workflow with the recorded
`reports/consumer-790-config.json` in the owning lane. This requires an actual
GCC compiler; a `gcc.exe` Clang compatibility alias is insufficient. Each phase
retains its binary, dependencies and compiler configuration. The three profile
paths are checked before training; missing or stale profiles are not accepted.
Use `pgo-ready --family 790 --phase baseline|generate|use` after each successful
phase. The corresponding policies are `reference`, `train` and `native`.
Training is not a performance sample. A bounded boot recording does not
establish representative gameplay coverage or a general title speed gain.

`anchor-ready --family anchor` selects the original modern executable. The
anchor workload records 600 frames from the state at guest vblank 120.
Use `run --family anchor --sample NUMBER` to record. Then add
`--playback PATH` to load and play that recording. The result retains the
runtime verdict, state digest count, RAM differences and ending guest cycles.
This compares a native recording with its own build, not a console oracle.

`tools/preserved_consumer_verify.ps1` can rebuild the paired consumer separately
with `-BuildName n`; the default `b` selects the original development build.
Use the same build name for Configure, Build, ModuleCheck, Record and Play.
Record and Play must also share their attempt number. Player receipts use
`modern-player-MODE-BUILD-ATTEMPT.json` so an earlier build's recording is not
silently selected. The wrapper checks the current Eagle host and its own
resource claim before each payload. The `n` build keeps the original `b`
executable, objects and benchmark log available for the earlier comparisons.

The PowerShell driver also accepts `runtime-configure`, `runtime-build`,
`runtime-test` and the corresponding `recompiler-*` phases. These use separate
source test trees, one compiler or test at a time, and existing local dependency
snapshots. Keep a new `-Attempt NUMBER` for every run. Configuration and build
success do not establish that every test passed. Review registered test commands
for window, audio and resource requirements before a test run.

`-Phase disc-geometry` builds an isolated native reader client from existing
reader/dependency objects. It checks every data sector, the volume descriptor
and boot files against independently extracted bytes from the approved local
Ape disc. A derived cooked view preserves sector positions; it does not prove
XA/audio/raw-sector equivalence or that the cooked image is playable. Keep the
result and native probe log before removing its owned derived image.
The driver uses installed CTest if the project SDK does not contain it.
`-ExcludeTests REGEX` limits the current pass and records that expression in
its command. Excluded tests remain unqualified. The driver selects dummy audio
and puts the recorded compiler's runtime libraries first on PATH for tests.
`-Target NAME` builds one source-tree target. `-IncludeTests REGEX` selects
tests for a focused check. Preserve the original failure list before a focused
CTest pass, because CTest replaces its last-failure record.
For Ninja builds, `-KeepGoing` collects independent failures in one diagnostic
pass. The pass still returns a failure code. It does not qualify failed targets.
The Eagle driver records the foreground, available slots, free memory, its
existing resource claim and source commit before each payload. Its build floor
uses the measured peak plus a 1 GiB margin. It does not create a resource claim.

Detailed results and limitations belong to the owning lane's
`reports/AUTONOMOUS-CONSUMERS.md` and `reports/consumer-*/` records. Inputs and
recordings remain private. The supported Studio package is unaffected.
