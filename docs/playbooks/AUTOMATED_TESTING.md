# Automatic accuracy testing

Every new compiled title should produce a test receipt alongside its build receipt.
Use tool-assisted speedruns (TAS), memory-card routes, and focused component tests
together. A passed TAS proves a specific route under specific conditions.
It does not establish a percentage of whole-game or hardware accuracy.

## Current implementation

This repository provides the [Tekken setup and replay tools](../../tools/tasreplays/README.md)
and the [qualified TAS comparison](../TAS_ACCURACY.md).
The native runner validates original inputs, complete RAM/clock observations,
and a bounded ending. The visible run --speed 32 or --speed max option sets a cap; run --headless is already uncapped. Measure actual elapsed time separately.

A separate private portfolio harness currently performs input admission,
repeat checks, coverage reporting, and sequential speed trials.
That harness is outside this source checkout. Save-route adapters and automatic
integration with every title build are not yet supplied by this repository.
The remaining sections define the expansion plan and acceptance rules.

## Per-build gate

1. Record the source commit, compiler, generated-code identity, and executable hash.
2. Run source-owned component regressions for the changed subsystems.
3. Bind the title revision, BIOS, emulator core/version, input, cards, and settings.
4. Boot each case from its declared reset or fresh card load.
5. Check progression and behavior at declared observation points.
6. Repeat each case from a clean start and compare its deterministic outputs.
7. Report failed checks and missing requirements separately from build success.

A shared runtime change should test every registered affected title.
Changes to CPU, scheduling, memory, or BIOS dispatch should default to all
registered titles. Use narrower selection only with recorded dependency evidence.
A missing title suite is an explicit gap; it must not become an implicit pass.
This integration policy is the target for build entry points, not a claim that
all existing build scripts already implement it.

## Test layers

| Layer | Required observation | Typical coverage |
|---|---|---|
| Component programs | Hardware-qualified result or documented reference trace | CPU/GTE instructions, exceptions, interrupts, DMA, timers, GPU, SPU, MDEC, CD and serial devices |
| Original full TAS | Unchanged input plus qualified state boundaries and ending | Connected progression and accumulated timing differences |
| Progression cards | Fresh boot/load, explicit markers, then controlled gameplay | Later areas and normal persistence |
| Short alternate routes | Specific action and expected effect | Death, retry, pause, menus, optional paths, unusual resources |
| Media samples | Pixel/VRAM and audio evidence at aligned points | Missing layers, movies, music, dialogue and timing |
| Lifecycle and endurance | Repeated transitions plus terminal state | Long sessions, save/reload, disc changes and resource growth |

Component tests must exercise the relevant path through the recompiler and
runtime, not merely a standalone helper with a similar implementation.
For generated and fallback execution paths, retain separate evidence where the
path matters. Reduce title failures into small authored programs when possible.
Keep the original title route as an integration regression.

## Reference selection

Replay each TAS in the exact source core and version first.
Pin firmware, disc topology, controller mode, card state, reset mode, and sync settings.
Confirm that the source replay reaches the intended endpoint before admitting it.
Tekken's Octoshock 2.2.2 compatibility work does not qualify another core version.
The selected [Pepsiman movie](https://tasvideos.org/6044S) uses Octoshock 2.3.0;
the selected [RE1 movie](https://tasvideos.org/8957S) uses Nymashock 2.9.1.

Native replay can work when the necessary timing is reproduced, as the Tekken
route demonstrates. A desynchronization is useful evidence, but it is not by
itself proof of a specific hardware bug. Find the first divergent input poll,
clock, state, or device event before assigning ownership.

A second independent emulator can help isolate a disagreement.
Agreement among emulators does not overrule measured hardware behavior.
Track source-emulator compatibility separately from hardware accuracy.

## Memory cards

Start with four to eight progression saves where the game supports them.
Choose saves by system coverage as well as distance through the story.
Include early gameplay, a transition-heavy area, a boss, and late-game state.
Add alternate characters, inventories, or routes when they change behavior.

Keep raw 128 KiB card images and record the original file and conversion recipe.
Verify the card structure and the title's actual ability to load it.
Region, revision, and save identifiers matter; renaming a file or identifier
does not establish compatibility. Give every run an independent writable copy.

A card contains persistent saves, not the complete machine state.
To make reference markers, boot the oracle and load that card through the game.
Observe the resulting area, flags, inventory, health, and player control.
Use the same procedure in the candidate. Arbitrary emulator save states are
not portable runtime checkpoints.

A TAS can produce useful cards only when it performs saves or when a separately
recorded save route is added. Many speedruns skip saving entirely.
Keep the original TAS unchanged; qualify the save-producing route separately.
Keep BIOS/card component checks in parallel with route acquisition, so one
shared card failure does not waste a full title campaign.

## Comparison rules

Use exact clocks and RAM when equivalent execution boundaries are established.
Use declared semantic markers and bounded waits for fresh save loads whose
complete initial machine state is not identical. A bounded wait is part of
that test; it must not silently retime an original TAS.

Keep timing-sensitive reads, instrumentation overhead, and capture settings explicit.
Read-only observers should not patch guest memory to manufacture a milestone.
Function-level comparisons need all relevant memory, device state, and side
effects in their contract; register inputs alone are insufficient for I/O code.

Keep correctness tests at the original presentation settings.
Exact software-renderer comparisons are useful where the reference permits them.
Enhanced rendering needs separate expectations and declared tolerances.
Audio deserves real sample or feature checks, including channel activity,
clipping, duration, transitions, and synchronization; silence detection alone
cannot qualify sound correctness.

## Speed and test budgets

Alex deferred performance work on September 9, 2026.
The active campaign expands TAS and save coverage at the current execution speed.
The speed rules below apply only to a later performance campaign.


First prove the route twice and measure its unbounded throughput. Test 1x, 8x, 16x, and 32x only while measured throughput supports the next target. Stop after an unattained target and improve actual throughput before testing higher caps.
Change host pacing only; retain every guest operation and original input boundary.
Record requested and achieved speed, guest cycles, elapsed host time, affinity,
priority, diagnostic settings, and completion. Repeat each speed setting.
If throughput misses its target, report it as unattained even if state matches.

Keep cheap component and short route tests in the per-build gate.
Run full registered routes for qualification; use longer scheduled endurance
campaigns only after the routes are stable. A schedule must identify its source
and fixtures. This document does not create a background schedule or CI job.
Keep retail data and private state captures on authorized private test hosts.
Source-owned tests can run in public CI.

## Failure handling and coverage

Retain the first divergent observation, exact inputs, source/build identity,
terminal state, and the shortest known reproducer. Distinguish a setup failure,
reference desync, candidate mismatch, timeout, and missing coverage.
Consult the shared findings before adding instrumentation or changing a model.
Make each verified fix a reusable regression and rerun affected title routes.

Track systems, content branches, persistence, presentation, and lifecycle as
separate requirements. Count executed code only as supporting coverage evidence.
A single speedrun can bypass most characters, weapons, saves, deaths, and menus.
User reports should become deterministic regressions where practical.
The size of the remaining human-only coverage is unknown until this matrix is populated.

## Implementation order

1. Retain the qualified Tekken replay as a regression for shared runtime changes.
2. Qualify Pepsiman's original source replay, then its native route.
3. Add RE1's exact Nymashock replay and the first fresh-load save adapter.
4. Add alternate paths, card lifecycle, and media checks to those title matrices.
5. Attach the shared admission/report contract to each supported build entry point.
6. Expand long-session and multi-disc coverage before admitting an FF7 full run.

The [TAS accuracy record](../TAS_ACCURACY.md) owns the current replay claim.
The [runner guide](../../tools/tasreplays/README.md) owns supported commands.
Private portfolio contracts retain campaign receipts, coverage gaps, and host
performance admission; this plan does not change a title qualification state.


