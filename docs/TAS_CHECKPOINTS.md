# TAS checkpoint diagnostics

`tools/tasreplays/run_native.py --save-state-at 300 301 360 361` captures at
the selected source GPU frontend returns. `--resume-from FILE.pst` resumes
the same route from a checkpoint. This is a diagnostic path: final TAS
qualification still uses an uninterrupted run against the independent oracle.

Every title command (`tekken3.py`, `pepsiman.py`, `biohazard.py`, `megamanx5.py`,
`redc.py`, `megamanx4.py`, `abesoddysee.py run`) takes the same options:
`--save-state-at RETURN...` and `--save-state-every N` (expanded to the multiples
inside the run's endpoint) capture during a normal replay without changing what it
qualifies; `--resume-from STATE [--resume-compatible-build]` replays only the
returns after the checkpoint, compares them against the source reference from K+1,
and is always recorded as `diagnostic`. A resume must use the same run length
(`--returns`) as the capture, because the checkpoint binds the route file's content.
None of them combine with `--ladder`.

The checkpoint manifest binds the exact executable, resolved configuration,
route, BIOS, game entry, guest clock and RAM digest. By default, a checkpoint
from another executable is refused. `--resume-compatible-build` permits a
diagnostic runtime rebuild with the same checkpoint compatibility identifier.
The loader still checks the state version, codegen hash/ABI, device sections,
configuration, assets and route. It verifies the saved file's SHA256 before load.
Each run uses a new output directory.

For repeatable cross-game qualification, `verify_checkpoint_cohort.py` accepts
`--run-args OPTIONS.json --output NEW_DIRECTORY --terminal 3000`. The JSON file
contains the normal `run_native.py` option strings, including the executable,
route, assets and profile, without an output directory or save/resume options.
The route and neutral tail must end at the requested terminal return. The driver
requires committed source, records two cold runs, then independently resumes
500, 1500 and 2500. It compares every CPU/clock row, every RAM-page hash and all
later saved sections. A timer-2 corruption must produce a state mismatch.
Use `--midpoint-only` with the full route and its terminal return for a separate
cold run and midpoint resume. Results, commands and raw evidence stay together
in the output directory; any missing evidence fails the cohort.

## Format and continuation

Boot-state format **v10** requires a 580-byte CPU section, a 36-byte CPU_EXEC
section, a 16-byte SPU sample-clock extension, and MDEC snapshot version 2.
It rejects earlier boot states and player save slots.

v10 is the merge of two lineages that both numbered their formats v6-v9 with
different contents: the resume lineage (`pegasus-codex/biohazard-resume-20260913`)
and upstream (per-word DMA2 linked-list progress, XA DATA_END, and the optional
enhancement-memory `MODMEM` section at tag 0x11 with its layout cookie in the
header's reserved word). Upstream keeps both. The resume lineage's scheduler
continuation therefore moved from 0x11 to `SCHED` 0x18, and its game-start latch
moved out of the header into the required 4-byte `BOOTFLOW` section, 0x19.
Tags 0x12-0x17 keep their resume-lineage numbers.

The merge also serializes state the resume lineage never had: the open poly-line
in GPU_SERVICE (384 bytes), the Nymashock drive's sub-Q latch, the exact timed-lid
deadline and pending lid IRQ in the CD section, and the SIO IRQ sequence counter
that IRQ_TIMING's `last_sio_seq_seen` is compared against. A restore also bumps the
device generation that keys the precise-slice deadline caches.
The operator superseded the earlier compatibility hold on 2026-09-12:
**development may break existing player slots; a v6/v7 reader is not required**.
Rewind and netplay share this loader. TAS checkpoints default to same-binary
identity checks. Compatible-build resume is opt-in and does not prove that a
state from before a fix equals the new build's cold execution at that point.
Changes affecting earlier execution require recapture or a cold comparison.

The header preserves whether the game-start transition has already run. Restore
sets this latch without repeating the boot-only RAM clears or CD speed change.
GPU_SERVICE includes all 32 queued command words. Source pad snapshots
retain the ACK pulse, pending timed ACK, and latched controller inputs. The source
DualShock/card profile also retains analog-mode locks and config capability;
the existing card section retains both slots' protocol machines and buffers.
Source multitap capture is refused because its full response is not represented.
The source
CD profile retains all eight sector buffers and absolute deadlines; source CDDA
also retains its two-sector audio pipeline and pending response. Source MDEC
retains its decoder FIFOs, partially decoded block, chroma buffers, DMA transfer
positions and service clock. Host callback pointers remain local to the binary.
SPU snapshots also retain the CD input audio ring, its cursors and counters;
queued audio affects the guest-visible sound-RAM capture buffers after resume.
The CD controller retains both pending and applied audio volume matrices.
Active CD DMA retains its service budget and absolute clock deadlines.

A required module with no serializer makes the save fail. It cannot create a
successful checkpoint containing an empty device section.

CPU_EXEC records the current instruction, pending branch delay slot and target,
and pending load writeback. Resume enters the instruction interpreter before
returning to the normal dispatcher. The scheduler retains the restored live
CPU registers rather than loading their older suspended TCB copy. Checkpoint
capture never writes a boundary without a represented instruction continuation.
A frontend return can land inside a nested exception dispatch, whose host call
chain no single continuation describes (Mega Man X4 return 2500 lands in the
kernel handler at 0x1BC0). A request at such a return moves to the next return
that can be captured. The manifest records both `frame` (where it was taken) and
`requested_frame` (the earliest request it satisfies), `saved-states.json` maps
every request to its state, and a request still pending when the route ends is
reported invalid.

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

Memory card images live in host `.mcd` files, not in any boot_state section, so
manifest schema **psx-tas-stateio-v3** carries them: each present slot's
in-memory image is saved beside the state as `<state>.card1.mcd` /
`<state>.card2.mcd` and bound by `card1_sha256` / `card2_sha256` (`none` for an
empty slot). Resume refuses a slot that differs in presence or digest and imports
the images before the state loads, so a checkpoint taken after the guest saved
continues with that card rather than the route's initial image. v2 manifests,
which never recorded cards, are refused. Player save states are unchanged: their
cards stay host-persistent.

The manifest records `frame` and `input_consumed`
separately. They are different boundaries: the measured return-300 checkpoint
had already consumed 301 inputs. The loader seeks the recorded input position.
Resumed observation hashes cover only the newly delivered suffix, labeled
`resumed_inputs`; CPU and RAM capture validation requires every return after
the saved return through the declared terminal return. It does not manufacture
observations for the skipped prefix.

Resume also accepts checkpoints in the declared neutral-input tail. The route
reader stays at EOF and supplies neutral controller samples. Suffix input hashes
are empty when all original inputs were consumed before the checkpoint; this
does not claim to revalidate the skipped original inputs.

## Debugging a late mismatch

Use the same full route, endpoint and device options for capture and resume.
Add `--save-state-at 233500 233501 233560 239202` to capture before Biohazard's
known first mismatch at 233568. This is one uninterrupted capture run, not four
runs. Keep its executable, manifest and states as immutable evidence.

To repeat the suffix, add `--resume-from CAPTURE/tas-state-233500.pst` and save
only later returns (`--save-state-at 233501 233560 239202`). To test a rebuilt
runtime, also pass `--resume-compatible-build`. Each attempt needs a new output
directory. The launcher records the selected executable and resume option;
runtime output records both saved and running executable hashes.

First compare a same-build resume with the uninterrupted capture using
`compare_checkpoint_runs.py`: compare every remaining CPU/clock and RAM row,
plus full states at K+1, K+60 and the endpoint. Repeat with a compatible rebuild
and check a deliberate timer perturbation is detected. Only then use the state
for short fix/test iterations. A final uninterrupted comparison remains required.

The compatibility identifier in `source_tas_stateio.h` must change when a runtime
change alters saved-state representation or continuation meaning, even if its
section lengths remain the same. The option is not a universal old-state loader.

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

For the full suffix, `compare_checkpoint_runs.py BASELINE RESUMED --start 1500
--terminal 3000 --captures 1501 1560 2500 2501 2560 3000 --output result.json`
checks every CPU/clock record and RAM-page hash as well as all requested states.
Missing states, incomplete coverage and discontinuous return numbers fail.

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

Failed-load admission has a separate full-runtime integration gate:
[`state_load_atomicity`](../runtime/tests/state_load_atomicity/README.md).
Run it against the candidate's native consumer build in addition to CTest. It
checks version-10 raw/compressed states, normal and comparison profiles,
malformed sections, dependency order, and allocation failures. No guest code
runs. A refused load must preserve the serialized machine; passing a replay or
the section-codec tests alone does not establish that property.

## Scope

The results below describe their recorded experimental revisions. The durable
`checkpoint-status.json` and its hash-bound evidence set identify which gates
have been reverified at the recorded head. The operator authorized resuming the
Biohazard independent-source investigation after the known-game checkpoint tests
pass. The earlier disc-identity change remains reverted in this branch.

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

The initial verification used the reduced no-card profile. Source SIO card and
DualShock models still retain their capture/restore refusals. Digital-pad, CDDA
and MDEC checkpoint support is under cross-game verification on this branch.
The reduced profile is infrastructure evidence, not Biohazard TAS fidelity or
full-profile checkpoint qualification. GPU command queues are now serialized.
No public release or shared pin promotion follows from these diagnostic gates.

The experimental `--cd-drive-model nymashock-1.29.0` option admits checkpoint
diagnostics with its bound clock tape. Its saved state includes physical head,
target, deadline, reset phase and random-tape cursor. Component checks pass;
full Biohazard continuation qualification must be recorded separately.
See [Bio Hazard source comparison](NYMASHOCK_SOURCE_REPLAY.md#experimental-bio-hazard-drive-comparison).
