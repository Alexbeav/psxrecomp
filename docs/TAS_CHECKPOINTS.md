# TAS checkpoint diagnostics

`tools/tasreplays/run_native.py --save-state-at 300 301 360 361` captures at
the selected source GPU frontend returns. `--resume-from FILE.pst` resumes
the same route from a checkpoint. This is a diagnostic path: final TAS
qualification still uses an uninterrupted run against the independent oracle.

The checkpoint manifest binds the exact executable, resolved configuration,
route, BIOS, game entry, guest clock and RAM digest. Rebuild before recording
both sides of a comparison. A checkpoint from another executable is refused.
Each run uses a new output directory.

## Format and continuation

Boot-state format **v8** requires a 580-byte CPU section, a 36-byte CPU_EXEC
section, a 16-byte SPU sample-clock extension, and MDEC snapshot version 2.
The research branch currently rejects earlier boot states and player save slots.
That is an unresolved compatibility break, not an approved player migration
policy. **Merge and release are blocked pending the operator's v6/v7 player-slot
decision and validation of the selected policy.** Rewind and netplay share this
loader. Do not treat successful TAS replay tests as approval to discard old slots.

CPU_EXEC records the current instruction, pending branch delay slot and target,
and pending load writeback. Resume enters the instruction interpreter before
returning to the normal dispatcher. The scheduler retains the restored live
CPU registers rather than loading their older suspended TCB copy. Checkpoint
capture refuses a boundary without a represented instruction continuation.

Scheduler escapes clear the precise-interpreter mode abandoned by `longjmp`.
Otherwise later compiled blocks mistake that stale flag for a live interpreter
and skip precise entry. The source-profile slice gate also includes the next
GPU service deadline, and an already-pending frontend return is due immediately.
This keeps the instruction continuation represented across frontend returns
even when no guest interrupt is due.

The CPU section preserves multiply/divide and GTE completion deadlines and
load timing credits. Load preserves the exact GTE backing registers. SPU sample
budgeting resumes from its saved guest-clock watermark and remainder. MDEC
preserves its absolute last-decode timestamp, including the never-decoded sentinel.

Manifest schema **psx-tas-stateio-v2** records `frame` and `input_consumed`
separately. They are different boundaries: the measured return-300 checkpoint
had already consumed 301 inputs. The loader seeks the recorded input position.
Resumed observation hashes cover only the newly delivered suffix, labeled
`resumed_inputs`; CPU and RAM capture validation requires every return after
the saved return through the declared terminal return. It does not manufacture
observations for the skipped prefix.

## Verification

Compare two uninterrupted runs of the same binary first. Then compare their
K+1 and K+60 checkpoints with independently resumed runs:

```powershell
python -B tools/tasreplays/compare_boot_states.py A/tas-state-000301.pst B/tas-state-000301.pst
python -B tools/tasreplays/compare_boot_states.py A/tas-state-000360.pst B/tas-state-000360.pst
```

The comparator checks every serialized section and reports uncompressed byte
offsets. A matching checkpoint is followed by a comparison of the remaining
per-return CPU and RAM-page/clock observations. A controlled restore perturbation
must make a previously passing comparison fail; a refused or crashed run alone
does not establish detection by the comparator.

Portable checks:

```powershell
python -B runtime/tests/test_cpu_state_wire.py --cc gcc
python -B runtime/tests/test_boot_state_section_wire.py --cc gcc
python -B runtime/tests/test_source_tas_stateio.py --cc gcc
python -B runtime/tests/test_source_gpu_service_path.py --cc gcc
python -B runtime/tests/test_scheduler_precise_escape.py --cc gcc
python -B tools/tasreplays/test_compare_boot_states.py
python -B tools/tasreplays/test_native_input_identity.py
python -B tools/tasreplays/test_pepsiman.py
```

## Scope

The results below describe their recorded experimental revisions. The durable
`checkpoint-status.json` and its hash-bound evidence set identify which gates
have been reverified at the current Lane B head. Lane A (the independent-source
frame-464 investigation) remains parked; its disc-identity change was reverted
from this branch and requires separate review.

The 2026-09-12 reduced-profile ladder passed using the unchanged diagnostic
route with 6,001 input records and 6,000 captured returns. Two cold runs matched
at returns 300, 301, 360 and 361. Resume from 300 matched all 23 sections at
301, 360 and 361, and every CPU register/clock row and all 512 RAM-page hashes
at returns 301–6000 (5,700 rows). This qualifies that checkpoint and suffix;
it does not establish arbitrary checkpoint or cross-profile equivalence.

The follow-up multiple-checkpoint test used the unchanged first 3,001 inputs
of that route. One cold process saved at returns **500, 1500 and 2500**, kept
running between saves, and stopped after return **3000**. Three fresh processes
then loaded those original files independently. Each matched all 23 sections
at K+1, K+60 and return 3000, as well as every other later checkpoint captured
by the cold run. CPU/register/clock records and all 512 RAM-page hashes matched
for all 2,500, 1,500 and 500 remaining returns respectively, with no gaps.
A second cold run reproduced all ten saved states and all 3,000 observation
records. A negative control changed the restored timer-2 counter at 2500;
the process completed normally and the section comparison detected the change
at 2501, 2560 and 3000.

The first attempt exposed the stale interpreter-mode flag: captures at 1500
and 2500 refused instead of producing files. The scheduler escape regression
test exercises the real `setjmp`/`longjmp` path at O0/O2 and fails when its mode
reset is removed. Checkpoint readiness still depends on the profile and the
existing device-state guards; the three passing positions do not qualify
every possible capture point.

To repeat this sequence, use `--save-state-at 500 501 560 1500 1501 1560 2500
2501 2560 3000` with a route that ends after return 3000. After that process
exits, run `--resume-from` separately for each of the three original `.pst`
files, requesting only the later save positions. Compare the resulting states
with the original cold files and require complete observation coverage through
3000. Keep the executable, route and profile identical across all four runs.

The negative controls changed one restored field in each of RASTER,
GPU_SERVICE, TIMER_SRC, DMA_SRC and IRQ_TIMING. All completed and produced a
state mismatch. GPU budget debt made return 301 non-quiescent, so capture
correctly refused there; its comparator failure was measured at return 360.
The other controls were detected at return 301. The service-path regression
test also detects the inherited P7 logger's early return, which had skipped
GPU dispatch, draw-raster advancement and DMA service. The accepted ladder
was recorded after removing that logger and restoring normal service.

The initial verification uses the reduced no-card profile. Source SIO card/pad
models, source CDDA and source MDEC still retain their capture/restore refusals.
The reduced profile is infrastructure evidence, not Biohazard TAS fidelity or
full-profile checkpoint qualification. Source GPU command queues must be empty.
No public release or shared pin promotion follows from these diagnostic gates.
