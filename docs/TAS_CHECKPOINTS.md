# Checkpoint capture and resume

A debug feature for replays. A run saves a checkpoint at a chosen frame, and a later run starts from it and continues exactly as the first run would have. Use it to start a long replay part way through, or to show that a change does not alter a run.

It is off unless its environment variables are set, and `PSX_NO_DEBUG_TOOLS` builds leave it out. Player save states, rewind and netplay are unchanged: without the capture variables a state is byte for byte what the runtime always writes.

## Two paths

The runtime has two checkpoint paths, and the run picks one at start:

- **Source GPU model runs** (`PSX_GPU_DMA_MODEL=octoshock-2.2.2-bounded-quad`, which `run_native.py --gpu-dma-model octoshock-2.2.2-bounded-quad` sets) capture at frontend returns and resume the same input route from the saved return. This is the TAS diagnostic path: see [Replay checkpoints](#replay-checkpoints-run_nativepy) below.
- **Every other run**, including `run_native.py` with another GPU/DMA model, captures at debug-server frames and resumes without seeking an input route. `run_checkpoint.py` drives it: see [Debug-server checkpoints](#debug-server-checkpoints-run_checkpointpy).

`run_native.py` replays an input route, so its checkpoint options need the source GPU path. It prints `run_native: checkpoint load path: <path> (gpu dma model <model>)` before the run starts and records the path in `manifest.json` as `resume.load_path`: `stateio` under `--gpu-dma-model octoshock-2.2.2-bounded-quad`, `none` without `--save-state-at` or `--resume-from`. With any other model those two options are refused (`load path: refused`) before the product starts, because the debug-server path cannot seek the route. Pin I made no capture under another model either.

Both use the same variables, the same manifest schema and the same state format. The title commands whose own options select the bounded-quad model (`tekken3.py`, `biohazard.py`, `megamanx4.py`, `megamanx5.py`, `redc.py`, `crash.py`) use the replay path.

## What a checkpoint holds

A checkpoint is three kinds of file side by side:

- the state, `<name>.pst`: an ordinary save state (`runtime/include/boot_state.h`). Every state of the current format carries the sections a resume needs (`CPU_EXEC`, `SCHED`, `GAME_START`, `MEMCTRL`, `IRQ_TIMING`), and the loader requires them;
- its manifest, `<name>.pst.json` (`psx-tas-stateio-v3`, `runtime/include/source_tas_stateio.h`): the frame taken and the frame requested, the cycle count, a RAM digest, the configuration and executable hashes, the input route and the state's SHA-256;
- one image per inserted memory card, `<name>.pst.card1.mcd` and `<name>.pst.card2.mcd`, each hashed in the manifest.

An existing manifest is never overwritten.

## Environment

| Variable | Meaning |
|---|---|
| `PSX_TAS_SAVE_STATE_AT=<frame>[,<frame>...]` | Capture at these frames (debug-server path) or frontend returns (source GPU path). |
| `PSX_TAS_SAVE_STATE_PATH=<file.pst>` | Where to write. `{frame}` in the name becomes the six-digit frame taken. |
| `PSX_TAS_RESUME_STATE=<file.pst>` | Resume from this state at start. |
| `PSX_TAS_RESUME_MANIFEST=<file.json>` | Its manifest, if not `<state>.json`. |
| `PSX_TAS_RESUME_COMPATIBLE_BUILD=1` | Accept a rebuilt executable whose checkpoint compatibility id matches. |
| `PSX_TAS_PERTURB_RESTORE=<field>` | After an accepted resume, change one restored value: `ram`, `irq_timing`, `sched`, `timer2` or `memctrl`. For negative controls only. |

On the debug-server path, frames are the debug server's frame counter, one per presented VBlank. A capture requested at a frame is taken at the next save-state boundary with a safe resume PC, which may be a frame or more later; the manifest records both. A frame requested while an earlier capture is still waiting is skipped.

## Resume checks

Before anything is loaded, a resume compares the manifest with this run: the configuration, the executable (or its compatibility id), the input route, the state file's SHA-256, and each memory card's presence and SHA-256. After loading it checks the frame, the cycle count and the RAM digest. Any difference prints `resume rejected: <reason>` and the process exits with status 2.

A resume never writes the memory card files bound to the run. The two paths treat the card images differently:

- on the replay path, a resume imports the checkpoint's card images (the `.mcd` files beside the state) before the state loads, so a checkpoint taken after the guest saved continues with that card rather than the route's initial image. An image whose presence or digest differs from the manifest is refused;
- on the debug-server path, a resume does not import: insert cards that already hold the checkpoint's images (copy the `.mcd` files beside the state into place first). A card whose presence or digest differs is refused.

Manifests of schema v2, which never recorded cards, are refused.

On the debug-server path, when `PSX_INPUT_ROUTE_FILE` is unset the route is recorded as `none`, and a resume does not seek an input route to the checkpoint's frame. Resumes of runs with recorded input use the source GPU path, which seeks the route (see [Frame and input position](#frame-and-input-position)).

## Debug-server checkpoints (run_checkpoint.py)

`run_checkpoint.py` runs a debug build headless with its debug server on. Give the game command after `--`; the driver adds the variables.

Capture at frames 500 and 3000, then resume from the first capture:

```console
python3 tools/tasreplays/run_checkpoint.py runs/A1 --save-at 500 3000 -- build/game --headless
python3 tools/tasreplays/run_checkpoint.py runs/B --save-at 3000 --resume-from runs/A1/tas-state-000500.pst -- build/game --headless
python3 tools/tasreplays/compare_boot_states.py runs/A1/tas-state-003000.pst runs/B/tas-state-003000.pst
```

`run_checkpoint.py` quits the game through the debug server once the capture at the last frame has its manifest, and writes `captures.json` and `run.json` in the output folder. Use the frame each capture was taken at, from `captures.json`, to pair the states. `compare_boot_states.py` names the first differing section and offset.

`compare_checkpoint_runs.py` and `verify_checkpoint_cohort.py` read the per-return CPU and RAM records that `run_native.py` writes; they do not read `run_checkpoint.py` output. `tools/tasreplays/test_checkpoint_tools.py` (CTest `checkpoint_tools_test`) covers `run_checkpoint.py` and `compare_boot_states.py` against a stand-in game; it does not cover those two tools.

## Replay checkpoints (run_native.py)

`tools/tasreplays/run_native.py --save-state-at 300 301 360 361` captures at the selected source GPU frontend returns. `--resume-from FILE.pst` resumes the same route from a checkpoint. This is a diagnostic path: final TAS qualification still uses an uninterrupted run against the independent oracle.

Every title command (`tekken3.py`, `pepsiman.py`, `biohazard.py`, `megamanx5.py`, `redc.py`, `megamanx4.py`, `crash.py`, `abesoddysee.py run`) takes the same options: `--save-state-at RETURN...` and `--save-state-every N` (expanded to the multiples inside the run's endpoint) capture during a normal replay without changing what it qualifies; `--resume-from STATE [--resume-compatible-build]` replays only the returns after the checkpoint, compares them against the source reference from K+1, and is always recorded as `diagnostic`. A resume must use the same run length (`--returns`) as the capture, because the checkpoint binds the route file's content. None of them combine with `--ladder`.

The checkpoint manifest binds the exact executable, resolved configuration, route, BIOS, game entry, guest clock and RAM digest. By default, a checkpoint from another executable is refused. `--resume-compatible-build` permits a diagnostic runtime rebuild with the same checkpoint compatibility identifier. The loader still checks the state version, codegen hash/ABI, device sections, configuration, assets and route. It verifies the saved file's SHA-256 before load. Each run uses a new output directory.

For repeatable cross-game qualification, `verify_checkpoint_cohort.py` takes `--run-args OPTIONS.json --output NEW_DIRECTORY --terminal 3000`:

```console
python3 tools/tasreplays/verify_checkpoint_cohort.py --run-args options.json --output runs/cohort --terminal 3000
```

The JSON file is a list of the normal `run_native.py` option strings, including the executable, route, assets and profile, without an output directory or save, resume or perturb options. The terminal return must be above 2560, and the route and neutral tail must end at it. The driver requires committed source, records two cold runs, then independently resumes 500, 1500 and 2500. It compares every CPU/clock row, every RAM-page hash and all later saved sections. A timer-2 corruption must produce a state mismatch. Use `--midpoint-only` with the full route and its terminal return for a separate cold run and midpoint resume. Results, commands and raw evidence stay together in the output directory; any missing evidence fails the cohort.

A cohort pass shows that resumes of that build reproduce its own runs. It does not compare the build with another emulator or with hardware.

## Format and continuation

The state format is `BOOT_STATE_VERSION` 18 (17 without trusted-title host provider state); the loader reads 17 and 18 and rejects every earlier state and player save slot. The CPU section is 592 bytes, `CPU_EXEC` is 36 bytes, the SPU section ends with a 20-byte sample-clock tail, and MDEC snapshots are version 2 (version 1 still loads).

MDEC restore reserves input storage for both the saved data and the rest of the active command. It refuses an expected input length above the existing snapshot input limit. It also refuses an output position beyond the saved output size. Output storage covers that size before the saved bytes are read. These bounds apply to both prepare and read without changing the wire format.

### Section tags

The format merged two lineages that both numbered their formats v6-v9 with different contents: the resume lineage and upstream (per-word DMA2 linked-list progress, XA DATA_END, and the optional enhancement-memory `MODMEM` section at tag 0x11 with its layout cookie in the header's reserved word). Both are kept. The resume lineage's scheduler continuation therefore moved from 0x11 to `SCHED` 0x18. Its game-start latch first moved out of the header into a 4-byte `BOOTFLOW` section at 0x19; that tag is now reserved, and `GAME_START` (0x12, u32 flags, bit 0 = game-start transition already applied) holds the latch. 0x14 (`CPUTIM`) is likewise reserved: the full CPU section holds that timing state. Tags run from 0x01 to 0x1F, and the presence mask is 32 bits, so a new section needs a format change.

### What the device sections keep

The format also serializes state the resume lineage never had: the open poly-line in GPU_SERVICE (384 bytes), the Nymashock drive's sub-Q latch, the exact timed-lid deadline and pending lid IRQ in the CD section, and the SIO IRQ sequence counter that IRQ_TIMING's `last_sio_seq_seen` is compared against. A restore also bumps the device generation that keys the precise-slice deadline caches. Rewind and netplay share this loader. TAS checkpoints default to same-binary identity checks. Compatible-build resume is opt-in and does not prove that a state from before a fix equals the new build's cold execution at that point. Changes affecting earlier execution require recapture or a cold comparison.

Restore sets the game-start latch without repeating the boot-only RAM clears or CD speed change. GPU_SERVICE includes all 32 queued command words. Source pad snapshots retain the ACK pulse, pending timed ACK, and latched controller inputs. The source DualShock/card profile also retains analog-mode locks and config capability; the existing card section retains both slots' protocol machines and buffers. Source multitap capture is refused because its full response is not represented. The source CD profile retains all eight sector buffers and absolute deadlines; source CDDA also retains its two-sector audio pipeline and pending response. Source MDEC retains its decoder FIFOs, partially decoded block, chroma buffers, DMA transfer positions and service clock. Host callback pointers remain local to the binary. SPU snapshots also retain the CD input audio ring, its cursors and counters; queued audio affects the guest-visible sound-RAM capture buffers after resume. The CD controller retains both pending and applied audio volume matrices. Active CD DMA retains its service budget and absolute clock deadlines.

A required module with no serializer makes the save fail. It cannot create a successful checkpoint containing an empty device section.

### Instruction continuation

CPU_EXEC records the current instruction, pending branch delay slot and target, and pending load writeback. Resume enters the instruction interpreter before returning to the normal dispatcher. The scheduler retains the restored live CPU registers rather than loading their older suspended TCB copy. Checkpoint capture never writes a boundary without a represented instruction continuation. A frontend return can land inside a nested exception dispatch, whose host call chain no single continuation describes (Mega Man X4 return 2500 lands in the kernel handler at 0x1BC0). A request at such a return moves to the next return that can be captured. The manifest records both `frame` (where it was taken) and `requested_frame` (the earliest request it satisfies), `saved-states.json` maps every request to its state, and a request still pending when the route ends is reported invalid.

Scheduler escapes clear the precise-interpreter mode abandoned by `longjmp`. Otherwise later compiled blocks mistake that stale flag for a live interpreter and skip precise entry. The source-profile slice gate also includes the next GPU service deadline, and an already-pending frontend return is due immediately. This keeps the instruction continuation represented across frontend returns even when no guest interrupt is due.

The CPU section preserves multiply/divide and GTE completion deadlines and load timing credits. Load preserves the exact GTE backing registers. SPU sample budgeting resumes from its saved guest-clock watermark and remainder. MDEC preserves its absolute last-decode timestamp, including the never-decoded sentinel.

### Frame and input position

On the source GPU path the manifest records `frame` and `input_consumed` separately. They are different boundaries: the measured return-300 checkpoint had already consumed 301 inputs. The loader seeks the recorded input position. Resumed observation hashes cover only the newly delivered suffix, labeled `resumed_inputs`; CPU and RAM capture validation requires every return after the saved return through the declared terminal return. It does not manufacture observations for the skipped prefix.

Resume also accepts checkpoints in the declared neutral-input tail. The route reader stays at EOF and supplies neutral controller samples. Suffix input hashes are empty when all original inputs were consumed before the checkpoint; this does not claim to revalidate the skipped original inputs.

## Debugging a late mismatch

Use the same full route, endpoint and device options for capture and resume. Add `--save-state-at 233500 233501 233560 239202` to capture before Biohazard's known first mismatch at 233568. This is one uninterrupted capture run, not four runs. Keep its executable, manifest and states as immutable evidence.

To repeat the suffix, add `--resume-from CAPTURE/tas-state-233500.pst` and save only later returns (`--save-state-at 233501 233560 239202`). To test a rebuilt runtime, also pass `--resume-compatible-build`. Each attempt needs a new output directory. The launcher records the selected executable and resume option; runtime output records both saved and running executable hashes.

First compare a same-build resume with the uninterrupted capture using `compare_checkpoint_runs.py`: compare every remaining CPU/clock and RAM row, plus full states at K+1, K+60 and the endpoint. Repeat with a compatible rebuild and check a deliberate timer perturbation is detected. Only then use the state for short fix/test iterations. A final uninterrupted comparison remains required.

The compatibility identifier in `source_tas_stateio.h` must change when a runtime change alters saved-state representation or continuation meaning, even if its section lengths remain the same. The option is not a universal old-state loader.

## Verification

Compare two uninterrupted runs of the same binary first. Then compare their K+1 and K+60 checkpoints with independently resumed runs:

```powershell
python -B tools/tasreplays/compare_boot_states.py A/tas-state-000301.pst B/tas-state-000301.pst
python -B tools/tasreplays/compare_boot_states.py A/tas-state-000360.pst B/tas-state-000360.pst
```

The comparator checks every serialized section and reports uncompressed byte offsets. A matching checkpoint is followed by a comparison of the remaining per-return CPU and RAM-page/clock observations. A controlled restore perturbation must make a previously passing comparison fail; a refused or crashed run alone does not establish detection by the comparator.

For the full suffix of two `run_native.py` runs:

```powershell
python -B tools/tasreplays/compare_checkpoint_runs.py BASELINE RESUMED --start 1500 --terminal 3000 --captures 1501 1560 2500 2501 2560 3000 --output result.json
```

It checks every CPU/clock record and RAM-page hash as well as all requested states. `--terminal`, `--captures` and `--output` are required, and the output file must not exist. Missing states, incomplete coverage and discontinuous return numbers fail.

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

Failed-load admission has a separate full-runtime integration gate: [`state_load_atomicity`](../runtime/tests/state_load_atomicity/README.md). Run it against the candidate's native consumer build in addition to CTest. It checks raw and compressed states, normal and comparison profiles, malformed sections, dependency order, and allocation failures. No guest code runs. A refused load must preserve the serialized machine; passing a replay or the section-codec tests alone does not establish that property.

## Recorded results

The results below were recorded on earlier experimental revisions of the source GPU path, with an older state format. They were not repeated on this branch and describe those revisions only.

The 2026-09-12 reduced-profile ladder passed using the unchanged diagnostic route with 6,001 input records and 6,000 captured returns. Two cold runs matched at returns 300, 301, 360 and 361. Resume from 300 matched all 23 sections at 301, 360 and 361, and every CPU register/clock row and all 512 RAM-page hashes at returns 301–6000 (5,700 rows). This qualifies that checkpoint and suffix; it does not establish arbitrary checkpoint or cross-profile equivalence.

The follow-up multiple-checkpoint test used the unchanged first 3,001 inputs of that route. One cold process saved at returns **500, 1500 and 2500**, kept running between saves, and stopped after return **3000**. Three fresh processes then loaded those original files independently. Each matched all 23 sections at K+1, K+60 and return 3000, as well as every other later checkpoint captured by the cold run. CPU/register/clock records and all 512 RAM-page hashes matched for all 2,500, 1,500 and 500 remaining returns respectively, with no gaps. A second cold run reproduced all ten saved states and all 3,000 observation records. A negative control changed the restored timer-2 counter at 2500; the process completed normally and the section comparison detected the change at 2501, 2560 and 3000.

The first attempt exposed the stale interpreter-mode flag: captures at 1500 and 2500 refused instead of producing files. The scheduler escape regression test exercises the real `setjmp`/`longjmp` path at O0/O2 and fails when its mode reset is removed. Checkpoint readiness still depends on the profile and the existing device-state guards; the three passing positions do not qualify every possible capture point.

To repeat this sequence, use `--save-state-at 500 501 560 1500 1501 1560 2500 2501 2560 3000` with a route that ends after return 3000. After that process exits, run `--resume-from` separately for each of the three original `.pst` files, requesting only the later save positions. Compare the resulting states with the original cold files and require complete observation coverage through 3000. Keep the executable, route and profile identical across all four runs.

The negative controls changed one restored field in each of RASTER, GPU_SERVICE, TIMER_SRC, DMA_SRC and IRQ_TIMING. All completed and produced a state mismatch. GPU budget debt made return 301 non-quiescent, so capture correctly refused there; its comparator failure was measured at return 360. The other controls were detected at return 301. The service-path regression test also detects the inherited P7 logger's early return, which had skipped GPU dispatch, draw-raster advancement and DMA service. The accepted ladder was recorded after removing that logger and restoring normal service.

The initial verification used the reduced no-card profile. The reduced profile is infrastructure evidence, not Biohazard TAS fidelity or full-profile checkpoint qualification. No public release or shared pin promotion follows from these diagnostic gates.

The experimental `--cd-drive-model nymashock-1.29.0` option of `run_native.py` admits checkpoint diagnostics with its bound clock tape. Its saved state includes physical head, target, deadline, reset phase and random-tape cursor. Component checks pass; full Biohazard continuation qualification must be recorded separately. See [Bio Hazard source comparison](NYMASHOCK_SOURCE_REPLAY.md#experimental-bio-hazard-drive-comparison).

## Versioned SPU key-off behavior

The `run_native.py` option `--spu-keyoff-model octoshock-1.11.9` selects the measured older-provider startup key-off behavior. The default keeps the newer source behavior, including the Spyro 5523M K7 startup holdoff. The older model requires the existing source-profile GPU/DMA mode. It is a provider model, with no title, frame, or voice selection.

For a native checkpoint saved before this model had a name, the explicit `--resume-spu-keyoff-from unversioned` selector also requires `--resume-from`, `--resume-compatible-build`, and the older nondefault model. Admission recomputes the old configuration with only the newly added model field absent. Game, route, capture interval, other models, tape content, and saved machine identities retain their checks. An additional legacy tape path still requires its existing explicit path/SHA binding. The transition changes future key-off interpretation; it does not change saved voice or RAM data. The runtime prints the selected transition before guest execution.

New checkpoints bind the selected model in their configuration digest and restore normally without the transition selector. A unit-test match or an accepted checkpoint is not a replay pass. Start before the measured cause: the Warped 110731 key-on/key-off fault requires a native state preceding that pair, rather than a later state already holding the wrong envelope.

## Held instruction-cache snapshot data

New `BS_SEC_ICACHE` payloads contain 1024 little-endian tag words, 1024 held instruction words and 1024 validity bytes (9216 bytes total). Restore retains stale cached instructions even when their backing RAM has changed; it never patches RAM to reconstruct them. Invalid validity bytes or payload lengths are rejected before any checkpoint module commits.

Legacy 4096-byte tag-only payloads remain readable through their prior RAM reconstruction path. They cannot recover instruction data that was never saved. A matched saved frame therefore does not prove later restored behavior; use a measured viable earlier checkpoint when that old payload loses history.

A disabled instruction-cache model selects current RAM even when a warm line or loaded snapshot still holds instruction data. A not-yet-initialized selector resolves `PSX_ICACHE` before the first word, matching the fetch timing path.
