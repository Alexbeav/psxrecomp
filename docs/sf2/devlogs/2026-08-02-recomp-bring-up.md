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

## R1 overlay ownership investigation

The first post-bootstrap probe reached frame 5,607 with no registered overlay
candidate and 187,589,545 interpreter fallbacks. `Game_Main` received
`0x80142158`, inside the shared MENU/MOVIE resident-overlay window. Bounded CPU
write watches then localized the installation path:

- at frame 727 the BIOS handoff wrote the resident image pointer/size near
  `0x80142158`;
- at frame 729 the retail copy loop at `0x8001076C..0x80010778`, returning to
  `0x8002B494`, wrote the resident overlay near `0x80142150`;
- at frame 898 the same loop installed TITLE bytes near `0x8014B950`.

These are ordinary CPU stores from an embedded archive into addresses that
also belong to the original PS-X executable text range. They are not direct CD
DMA overlay loads. The original capture manifest contained only `0x80000000`
and `0x8000D000`, while the loader checked only physical bases `0x0` and
`0xD000`. Nevertheless, live state at `0x8011EE8C` became depth 2, state 4,
transition 0. TITLE was executing faithfully through the interpreter, but its
pages were structurally absent from capture.

The generic defect was in `text_guard_note_write`. A CPU write differing from
the registered static executable image marked `text_modified_bitmap`, which
correctly diverted live code to the interpreter, but did not mark the page in
`dirty_ram_bitmap`. Overlay capture serializes dirty-page runs, so CPU-installed
code inside the original text window could never enter capture-and-compile.
The fix marks that page dirty at the already-proven mismatching executable
write. Capture still requires execution evidence, and exact-range validation
continues to admit unaffected static functions, so data-only writes do not
become overlay candidates merely by touching the page. A structural regression
test protects this handoff. No SF2 address was added to framework code, and no
generated C or captured output was edited.

## Corrected capture and native cache

The CLI package was rebuilt, and a new ignored project was generated from the
same user-owned Disc 1 input at
`lab/sf2/local/generated-disc1-r1-text-capture`. Its Release instrumentation
build completed all 578 Ninja actions. A fresh capture at retail state 4
produced:

| Base | Bytes | Executed PCs | Dispatch targets | Seed targets |
|---:|---:|---:|---:|---:|
| `0x80000000` | 49,156 | 191 | 23 | 23 |
| `0x8000D000` | 4,100 | 15 | 3 | 3 |
| `0x8013E000` | 40,964 | 1,453 | 58 | 58 |
| `0x8014B000` | 180,228 | 1,656 | 25 | 25 |

The loader now checked the SF2 physical bases `0x141000`, `0x158000`,
`0x14B000`, and `0x13E000` in addition to the low-memory regions. Offline
compilation verified codegen hash `8d349ec4` and config hash `cd77ebe4`; all
four GCC shards built, with zero failed shards, unsupported-instruction TODOs,
or unknown/bad targets.

Clean cached runs loaded all four regions and registered 117 candidates. One
representative query reported 13,995,933 native-overlay dispatches and 17,416
interpreter fallbacks; another reported 14,224,275 and the same 17,416. Both
had zero stale blocks, zero CRC revalidation misses, and 132 range links. Live
CRC checks at `0x80143160` and `0x8014CCDC` matched their candidates. Static
resident dispatch reported zero misses. Retail state was depth 2, state 4,
transition 0 in every native run.

## R1 fixed-frame comparison

Two additional clean headless processes, each with an isolated memory-card
directory, returned identical frame-2500 records. The complete GPR array,
`COP0_SR=0x40000401`, `COP0_CAUSE=0x00000400`, `EPC=0x800F4950`,
`I_STAT=I_MASK=0xCD`, pad buttons `0xFFFF`, SIO status/control, and the enabled
`320x240`, 15-bit display at page Y=240 all matched. At the bounded
presentation query, both runs also reported exactly 7,573,945 GP0 writes, 983
draw commands, 4,196 environment commands, 3,738 copies, and draw area
`0,0..319,239` with offset `160,120`.

A live GPU-status query made much later in one process differed only in a
transient high status bit (`0xD6020200` versus `0x56020200`). It was not treated
as stable guest divergence: the fixed-frame record and bounded GP0 state match,
while the live queries occurred at different host/query times.

R1 therefore passes. The retail application reaches state 4 with the TITLE
region active in the native cache, the native resident/native overlay/
interpreter tiers are reported separately, and the guest/input/presentation
boundary reproduces across clean processes. The next gate is R2: drive the
retail frontend, exercise MENU/INIT overlays, prove reuse plus actual
invalidation, and measure remaining fallback by range.

## R2 native fasttrack divergence

The first retail START campaign proved that the pad word reached retail code,
but the all-native process stopped making useful progress after TITLE startup.
The GPU counter froze at exactly 7,573,945 GP0 writes while the CD controller
lost thousands of pending INT1 events and retried `Setmode`. With
`PSX_OVERLAY_NATIVE_OFF=1`, GPU and CD activity remained healthy.

A deterministic native-candidate rank bisection found rank 14 healthy and rank
15 broken. Rank 15 was owner `0x00003590`, CRC `0xBC0AD72A`, in cached shard
`00000000_CB2A51E3.dll`; it first executed at frame 732. The same compiled body
also exported aliases `0x35AC`, `0x35B4`, `0x3620`, `0x3628`, and `0x3694`, so
blocking only the owner address was insufficient. Rank 15 with the owner body
blocked was healthy.

Bounded interpreter instruction capture resolved the first semantic mismatch.
At `0x362C`, the OpenBIOS fasttrack handler executes:

```text
lw    k0,0x4c38(k0)
move  at,k0
```

On MIPS-I the `move` observes old `k0`; the loaded value becomes visible only
after that successor. The interpreter therefore preserves the old pointer in
`at` before storing the new queue pointer. Captured-overlay CFG emission instead
wrote `k0` immediately, so `at` received the new pointer and a later store
corrupted the CD fasttrack queue. The repository's separate full-function
emitter already documents and implements this exact OpenBIOS load-delay idiom;
only the CFG emitter used by captured overlays lacked the value semantics.

`CodeGenerator::translate_basic_block` now recognizes ordinary in-block
`LB/LBU/LH/LHU/LW` pairs whose immediate non-control-flow successor reads the
destination. It emits the load into a temporary, runs the successor against the
old register value, then commits the deferred value unless the successor itself
writes that register. Label/control-flow boundaries remain conservative. The
regression fixture uses the exact `lw k0; move at,k0; sw` sequence and verifies
the generated ordering.

The CLI package and a fresh ignored project were generated without editing
generated C. Its 578-action Release/debug-tools build completed. The original
four-region capture remained available in the immutable private capture
history even though later probes had replaced the canonical manifest. Both the
four-shard preflight and real rebuild succeeded under codegen hash `9713afe3`
and config hash `cd77ebe4`.

Two clean all-native processes passed the old ceiling:

| Run | Observed frame | GP0 writes | CD INT1 lost |
|---|---:|---:|---:|
| A | 3,861 | 22,085,066 | 0 |
| B | 4,055 | 23,973,334 | 0 |

Both continued rendering and reading sectors. The first representative tier
query loaded four regions and 117 candidates and reported 15,596,367 native
overlay dispatches plus 25,029 interpreter fallbacks. The old GPU ceiling and
CD-loss signature did not recur.

## R2 operable frontend and replacement lifecycle

The retail TITLE input window is its internal state 3 at `0x80156BDC`.
Synchronizing the existing debug-server pad override to that state and holding
active-low START (`0xFFF7`) produced the authentic `New Game` screen. Holding
active-low Cross (`0xBFFF`) selected `New Game`, then `One Player`. No state
write, skipped transition, or native frontend substitute was used.

The continuous retail route then showed the opening aircraft movie at
`512x240x24`, changed the application stack from depth/state `2/4` to `3/3`,
and eventually reached depth/state `2/8` with the `384x240x15` mission briefing
and its retail “Press X to continue” prompt. The title/menu itself remained
`320x240x15`. CD `int1_lost` stayed zero throughout.

An on-demand bounded capture at the movie boundary preserved two updated
regions. Offline preflight and real compilation both succeeded:

| Base / CRC | Bytes | Candidate definitions |
|---|---:|---:|
| `0x8013E000 / BA003DC3` | 40,964 | 84 |
| `0x8014B000 / 979FB883` | 663,556 | 61 |

Both had zero failed shards, unsupported-instruction TODOs, or unknown/bad
targets. A clean process then loaded all six cached region variants and
registered 242 candidates. Live candidate queries matched MOVIE entry
`0x80143A10` and TITLE entries `0x801538C4` and `0x801501F0`; the clean route
again reached state 8.

This second route also supplied the required non-inferred replacement evidence:
the loader reported one invalidation, one stale dispatch blocked, and one
revalidation CRC miss, reducing the valid candidate count from 242 to 241 while
execution continued safely. Aggregate state-8 tier counts were 46,643,785
native-overlay dispatches and 186,261 interpreter fallbacks.

Bounded hardware evidence at state 8 recorded 4,135 SPU key-ons and an 8,294
entry key-on/off event total, with active retail voices. CD history contained
2,340-byte raw sectors from the movie/briefing path. Headless audio rendering
was disabled and the sampled sectors reported no XA-audio delivery, so XA
output is not claimed yet.

The framework suite passes 38/38 with `PYTHONUTF8=1`; without that required
environment setting, three Python tests fail only on `cp1253` decoding. The
ignored local experiment footprint is 4.032 GiB, below the 20 GiB cap. The R2
frontend, capture/rebuild, cache reuse, and actual invalidation gates are now
demonstrated. The next section completes fallback attribution and replacement
coverage; XA evidence and a fixed-checkpoint repeat remain before selecting
Mission 3 through retail state.

## R2 fallback attribution and stale-entry replacement

A third clean hidden/headless route reached the same retail mission briefing at
application depth/state `2/8`. At the bounded state-8 query, `dirty_ram_stats`
reported 268,338 attributed interpreter block dispatches. Grouped by physical
address band, the fallback share was:

| Physical band | Dispatches | Share |
|---|---:|---:|
| `0x141000..0x14AFFF` | 120,179 | 44.79% |
| `0x010000..0x0CFFFF` | 72,310 | 26.95% |
| `0x0D0000..0x13DFFF` | 71,775 | 26.75% |
| `0x158000..0x1DFFFF` | 4,035 | 1.50% |
| `0x14B000..0x157FFF` | 39 | 0.01% |

The dominant exact entries were `0x80142AE4`, `0x80142BD4`,
`0x80142B84`, `0x800F928C`, and `0x80022584`. None lies in a code range in
the six cached `.ranges` manifests. All relevant capture builds continue to
report zero unsupported-instruction TODOs and zero unknown/bad targets. The
measured fallback is therefore overwhelmingly unseeded/uncovered code, not a
recompiler opcode limitation. The interpreter recorded no aborts.

An unfiltered candidate-table query named the one real invalidation. Cached
candidate `0x8014C0F0` from the `979FB883` region expected code CRC
`1FC7107E`; state-8 RAM produced `4FCFFE46`, so the loader invalidated it and
blocked its stale dispatch. The current state-8 capture produced region CRC
`81E32E21` (663,556 bytes, 360 candidates). Focused preflight and real GCC/CPS
builds both succeeded with no failed shards, unsupported instructions, or bad
targets.

The stale address was an observed dispatch but was absent from the later
capture's promoted roots. The compiler's bounded `--force-interior 0x8014C0F0`
recovery path therefore built exact-demand fragment `0C2EF971`; generated C
and captures were not edited. A fresh clean process loaded seven region DLLs
plus that fragment. At state 8, the candidate table contained both the rejected
old entry and a valid replacement at `0x8014C0F0` whose stored and live code CRC
both equal `FF46C59F`. The route remained retail-owned and reached the
`384x240x15` briefing with 41,315,186 native-overlay dispatches, 149,119
interpreter fallbacks, and CD `int1_lost=0` at the first checkpoint query.

This completes fallback attribution and replacement-coverage evidence. XA
delivery/output evidence and a fixed-checkpoint guest-state comparison remain
before retail Mission 3 selection.

## R2 bounded XA delivery and output

Headless mode intentionally creates no SDL audio device and therefore never
calls the canonical SPU renderer. A fourth clean process was launched without
`--headless`, after announcing the visible runtime window, with
`SDL_AUDIODRIVER=dummy`. This preserves the full guest CD/XA, SPU render, and
host-output pipeline while sending host samples to a silent SDL sink. No WAV or
other audio payload was written.

The same retail-owned START, New Game, and One Player inputs reached application
depth/state `3/3` and the `512x240x24` aircraft movie. During that movie, the
bounded CD sector ring contained realtime XA audio sectors with file/channel
metadata, submode `0x64`, coding `0x01`, `data_delivered=0`, and
`xa_audio_delivered=1`. Adjacent video/data sectors remained separately tagged
with `xa_audio_delivered=0`, demonstrating that this is the decoder's delivery
classification rather than a generic streaming counter.

At the first movie query, the always-on PCM taps reported:

| Tap | Frames | Nonzero | Audible-threshold | Peak |
|---|---:|---:|---:|---:|
| SPU output | 2,925,300 | 1,463,014 | 1,393,808 | 24,623 |
| CD/XA input | 1,481,760 | 1,238,507 | 1,206,329 | 31,945 |
| Host output | 2,929,664 | 1,442,381 | 1,373,744 | 24,628 |

The host bridge was active at 44.1 kHz. A later bounded snapshot reported
3,305,295 SPU-rendered frames, 1,809,141 nonzero frames, 56,448 frames pushed
into the current CD FIFO accounting window, zero CD overflow frames, and a
`CD_PUSH` event followed by `RENDER` events in the sample-clocked ring. CD
streaming remained active with filter file/channel `1/11` and `int1_lost=0`.

This proves XA sector recognition, decoded nonzero PCM delivery into the SPU,
nonzero canonical SPU output, and bytes reaching the host API. It does not claim
physical-speaker audibility because the host sink was deliberately silent.
The remaining R2 gate before Mission 3 selection is the fixed-checkpoint
state-8 guest-state comparison.

## Upstream submission

With user authorization, the load-delay correction was extracted onto current
upstream `master` (`0cfa9fe`) in a separate ignored worktree. The submission
contains only `recompiler/src/code_generator.cpp` and the focused regression in
`recompiler/tests/recompiler_patch_test.cpp`; SF2 documentation, runtime
diagnostic environment hooks, captures, and generated artifacts were excluded.

The minimal commit is `5834990`. The prescribed recompiler build and complete
CTest suite passed 38/38 before publication. It was pushed through the user's
GitHub fork and opened as
[PSXRecomp PR #93](https://github.com/mstan/psxrecomp/pull/93). The PR is open;
GitHub checks had not started at the time of this handoff.

## Mission 1 dialogue-boundary JALR contract

A visible fixed-route attempt corrected an earlier overstatement about startup
coverage. After the 989 logo, the user observed no Eidetic logo, legal did play,
and `z_intro` did not play before the menu. The automation had armed START from
the first observation of `0x80156BDC == 3`. A later no-input trace proved that
word is not a globally unique TITLE gate: it also equals 3 around frame 945
while the display remains in a 24-bit startup-movie phase. Early injected input
can therefore skip or propagate across startup presentation. The missing clips
remain an observation to retest with a compound gate; they are not currently
attributed to the runtime.

The same visible route then halted at the Mission 1 post-FMV intro precisely
where dialogue should begin. The halt was active rather than a silent audio
wait. At frame 4,828 the recursion guard reported dispatch depth 257, with the
recent function tail alternating resident dispatcher `0x8002A094` and overlay
callback `0x801C63B4`. CD remained active on file/channel 1/11 with
`int1_lost=0`; SPU, decoded CD input, and host output all contained nonzero
samples. The active `0x8014B000/81E32E21` candidate matched its live code CRC.

A clean visible control with `PSX_OVERLAY_NATIVE_OFF=1` reproduced the same
failure at frame 10,145. Its dirty-block tail made the control-flow contract
explicit:

```text
0x80108BEC: jalr $a1,$t0  ($t0 = 0x80010000, $a1 = 0x80108BF4)
0x80010000..08: runtime-installed tail stub -> 0x8002A094
0x8002A094: consume descriptor at $a1 -> callback 0x801C63B4
callback returns through the pre-existing $ra
```

The dirty-RAM JALR path wrote `pc+8` to `rd ? rd : $ra`, so `rd=0` was already
architecturally wrong. It then ran all nonlocal targets through
`psx_dispatch_call(..., pc+8)`, whose call-unit contract requires `$ra ==
pc+8`. For SF2's nonstandard `$a1` link, `$ra` correctly remains the outer
return address. The runtime consequently fabricated a failed continuation on
each descriptor crossing and accumulated nested dispatch frames until the
guard fired.

The fix writes the encoded nonzero `rd` exactly and limits call-unit
optimization to `rd == $ra`. Other JALR link registers use the faithful flat
pc-chain, allowing the callee's eventual JR to choose the real continuation. A
registered source regression protects both `rd=0` and the non-`$ra` handoff.
The complete recompiler suite now passes 39/39.

The rebuilt private executable required a semantic, not frame-number, retest.
An interpreter-only automation run remained live beyond the old fatal frame,
but the ambiguous startup input gate meant that observation could not prove it
had crossed the same retail boundary. A native-overlay-on run likewise advanced
past frame 4,828 while still at the main menu; that initial inference was
explicitly rejected when the user identified the visible state.

The user then controlled that same patched native process through `New Game`,
`One Player`, and Mission 1. It crossed the exact post-FMV dialogue boundary
that had frozen before and entered live `384x240x15` gameplay. A bounded
post-boundary snapshot at frame 18,191 recorded 137,030,044 GP0 writes with
current world-3D activity, 98,979,844 native-overlay dispatches, 313,042
interpreter fallbacks, one real invalidation/stale block, and CD `int1_lost=0`.
This is the accepted fix validation.

The process used `SDL_AUDIODRIVER=dummy`, so it was intentionally silent at the
physical output; the user confirmed hearing no sound. Nonzero CD/XA, SPU, and
host-buffer taps prove only internal sample flow, not audible output. Real-device
sound remains a separate validation item. No generated C or captures were
edited. The ignored local footprint is 4.351 GiB.

## Save deadlock: nested call-unit IRQ suppression

A subsequent visible run on the real audio device confirmed audible retail
output, crossed the Mission 1 dialogue boundary, and entered gameplay. Twice,
`Save and Quit` reached the memory-card UI but froze before returning to the
menu. The second run used `PSX_SAME_THREAD_RESTORE=2` and reproduced the same
boundary. Both 128 KiB card images retained valid `MC` headers and empty
formatted directory state.

The hardware path before the freeze was healthy: sequential reads completed
through sectors 0--31, BIOS events were delivered, SIO IRQ bit 7 had balanced
sets/clears, and no retail `0x57` write command occurred. This excluded card
formatting, persistence, and SIO write timing as the immediate blocker.

An initial callback-entry hypothesis was also rejected by direct evidence.
`0x80145360` is an exact manifest candidate (`F 80145360 F99BB31E`, size
`0x14`), not an unregistered interior entry. The native-call ring showed it
executing and returning repeatedly, including at the frozen frame. Dirty
interpreter records independently showed the paired hardware-event callback at
`0x80145310`. Retail `0x80145820` consumed those event flags, completed one
invocation, then began another and waited for the next event.

At frozen frame 4,500, the guest had an enabled pending IRQ (`I_STAT=0x41`,
`I_MASK=0x4D`, COP0 interrupt enable set), but the frame could not advance. The
runtime cause was generic: `overlay_loader_call_native()` increments
`g_call_unit_depth`, while both `overlay_ci_wrapper()` and
`overlay_ci_at_wrapper()` returned immediately whenever that depth was nonzero.
The outer native call was therefore treated as atomic against IRQ delivery even
when its retail callee legitimately waited for an IRQ-backed BIOS event. It
could not return until an IRQ that its own call scope suppressed indefinitely.

The correct invariant already existed one layer lower. `interrupts.c` permits
the guest IRQ handler to run inside a nested call unit; if the handler requests
a cooperative cross-thread switch, it restores the interrupted thread and sets
`s_defer_switch_pending`, applying the switch at the clean outer boundary. The
fix therefore removes only the two call-depth IRQ-suppression guards. Native
call units remain atomic with respect to thread changes, not interrupt delivery.
A registered source regression protects both wrapper delivery sites and the
deferred-switch machinery. The complete suite passes 40/40.

After the user closed the two old windows, the canonical private executable and
an independent clean build both linked. A clean headless process reached the
real `New Game` frontend before visible testing. The user then repeated the
retail Mission 1 route in a fresh visible native-overlay process with the real
audio device and an isolated memory-card directory. `Save and Quit` completed,
returned normally, and the resulting save loaded successfully.

The bounded post-load snapshot at frame 7,180 was live with no pending masked
device state (`I_STAT=0`, `I_MASK=0x4D`), 849 card probes/commands, 849 ACKs,
zero transaction aborts, and continued native-overlay IRQ delivery. The full
retained card transaction ring contained 849 closed transactions, including
120 successful `0x57` writes; representative directory-sector and data-sector
writes each reached terminal state 18 with 138 transferred bytes. `card1.mcd`
is 131,072 bytes, has a valid `MC` header and active directory entries, and has
SHA-256 `F3969716841AF868443FD6EDD7F1E14C02ACB5D9884CC47B45D7DEBB5AE27414`.
The user successfully loading the save is the semantic persistence check.

No SIO timing, card format, callback address, retail state, generated C, or
capture was changed. The ignored local footprint after the clean build and
validation evidence is 4.635 GiB.

## Open 24-bit FMV band corruption

The successful load exposed a separate presentation defect. During an FMV, the
decoded central movie rectangle was correct but unrelated colourful VRAM was
visible in the upper and lower bands. The user's screenshot demonstrates that
the stream itself is not corrupt; the presentation path is scanning uncovered
24-bit VRAM around the movie payload. Existing code only tracks and blackens a
trailing horizontal depth-24 upload margin. The vertical upload/display bounds
were not captured while the affected frame was live, so no correction is yet
claimed. The next run must record exact GP1 display coordinates and bounded
CPU-to-VRAM upload rectangles at the affected movie before changing the generic
present rule.

That capture is now complete. A bounded 128-entry depth-24 upload ring was added
because SF2 assembles movie frames from strips rather than one framebuffer-sized
upload. The diagnostic build passed the 40/40 framework suite, linked in a clean
private build directory, and reached the retail frontend headlessly before the
visible test.

On affected aircraft-movie frame 31,447, the CRTC scanned `512x240x24` from
display base `(0,0)`, horizontal range `615..3175`, vertical range `16..256`.
The retained uploads showed each decoded buffer built from 32 adjacent
`24x160`-halfword strips. The displayed buffer covered `x=0..767`,
`y=40..199`; the alternate covered `x=0..767`, `y=280..439`. Thus the correct
160-line movie payload is centered in a 240-line scanout and the visible junk is
exactly the unrefreshed 40-line band above and below.

An identical executable and copied save were then launched with
`--renderer software`. The user confirmed that the same FMV was correct. This
A/B excludes retail coordinates, MDEC decode, disc data, and the general
depth-24 byte unpacker. The remaining lead is OpenGL representation coherency:
`gpu_fill()` updates only the FBO, `sw_fill_rect()` updates CPU VRAM, and
depth-24 presentation intentionally reads CPU VRAM without a blanket FBO sync
because the packed RGB888 movie strips already live there. A pre-movie black
clear can therefore be correct in the FBO but stale in the CPU mirror outside
the 160 uploaded lines.

The generic correction is not yet implemented. A narrow GP0 transition capture
must first prove the exact retail fill/copy operation. An attempted broad scan
of historical GP0 frames requested too much data, tripped the debug-server
starvation guard, and exited the diagnostic process after the affected frame
and upload ring were safely captured. This is recorded as a tooling/query
failure, not a game crash. The bounded upload telemetry is commit `4b5edc7`.

The user reports one complete end-to-end Mission 1 playthrough with correct
behavior for the exercised route, including real-device audio, save, and load.
This is accepted as a functional feasibility milestone, but the comparison
protocol still requires two clean deterministic checkpoint runs before the
route is called reproducible.

## Portfolio corpus consultation

The private `Alexbeav/PSX-Ports` knowledge corpus at aggregate commit
`534097e` was consulted under its mandatory consult-test-return workflow. The
search covered the stable findings registry, implementation candidates,
failure catalog, CPU/device/GPU/validation contracts, regression ledger, and
the SF2 hybrid, SF2 recomp, SF3, and Tenchu normalized project reports. Search
terms included the visible symptom, depth-24 scanout, stale bands, CPU/FBO VRAM
mirrors, fill/copy coherence, and split representations.

Lead dispositions:

- Candidate `PSX-GPU-002` and `FAIL-009` exactly match the normalized symptom
  and likely owner, but both derive from this lab's evidence. They confirm the
  corpus classification; they do not supply an independent implementation or
  fix.
- Stable `PSX-GPU-001` is a relevant constraint rather than the direct cause:
  PS1 VRAM and display pages persist until retail overwrites them. A blanket
  rule that blackens every uncovered 24-bit band would hide the coherency bug
  and could destroy valid authored persistence.
- `PSX-MDEC-004` is not applicable to this failure. The identical retail input
  and executable decode/present correctly under the software renderer, so the
  quantization/container path is not the first divergence.
- The SF3 caller-qualified capture and same-tick composition findings are
  useful future validation patterns but do not explain this CPU/FBO mismatch.
- The regression ledger confirms that no title-neutral fill/copy/15-to-24-bit
  coherency fixture exists yet.

The next falsifiable check remains narrow: capture the exact GP0 fill/copy and
GP1 depth transition immediately before the first affected movie strips, then
test whether mirroring only the demonstrated authoritative operation into CPU
VRAM makes OpenGL agree with the already-correct software control. Do not import
an SF2 dimension, movie identity, or unconditional black-band policy.

Tenchu is the first proposed independent consumer because its private
PSXRecomp lane already runs a retail intro FMV and can repeat the OpenGL versus
software check on a different title. SF3 is the next presentation-contract
consumer as its renderer composition matures. A confirmed generic fix,
source-owned regression, project report update, candidate update, failure
disposition, and next-consumer result must be returned to the corpus; stable
promotion remains blocked on the portfolio-wide 27-candidate classification
pass.

## OpenGL depth-24 ownership correction

The bounded GP0/GP1 transition proved the generic split-representation defect.
OpenGL made the FBO authoritative for 15-bit fill/draw/copy work, then retail
switched to depth 24 and uploaded packed movie strips into CPU VRAM. Presentation
read the CPU representation, so unuploaded rows exposed stale data even though
the FBO contained the correct black fill.

Commit `09be64b` adds a renderer depth-change hook. OpenGL performs `ensure_cpu()`
at the 15-to-24 handoff before movie uploads, mirrors fills while packed depth-24
CPU VRAM owns scanout, and applies ownership policy before CPU uploads. The fix
does not encode movie dimensions, bands, SF2 addresses, or output presentation.
`gl_depth24_coherency` covers fill/copy/upload ownership. The framework suite
passed 41/41 at that milestone. Hidden OpenGL changed from 292 mismatches in the
affected region to zero, while software remained correct. The user independently
confirmed the formerly corrupt FMV presentation was correct.

Corpus results were recorded as follows: `PSX-GPU-002`/`FAIL-009` confirmed;
`PSX-GPU-001` narrowed to the persistence constraint; `PSX-MDEC-004` irrelevant;
hardcoded clearing rejected. Tenchu remains the first independent consumer.

## Authentic startup CD seek lifecycle

A fully neutral no-input native run still skipped Eidetic and ZINTRO, so the
earlier ambiguous TITLE trigger was not the root cause. Three bounded checks
rejected input propagation, overlay-native generation, and movie/container
identity. Exact command/sector history showed retail issuing SetLoc/SeekL for
Eidetic at frame 1295, but the CD device continued delivering the old 989 read
stream.

SeekL/SeekP did not stop an active ReadN/ReadS generation. Commit `485b79b`
calls `stop_read_stream()`, clears READ/PLAY state, and then enters SEEK. The
title-neutral `cdrom_seek_retarget` regression proves that old-location sectors
and pending data-ready ownership cannot survive the retarget. The framework
suite passed 42/42.

The corrected interpreter and native routes both produced the exact authentic
sequence: 989 frame 925, Eidetic 1268, legal 1422, ZINTRO 1752, TITLE 18493.
`PSX-HLE-001` was narrowed to a complete-device-state lesson; the confirmed
owner is the LLE CD lifecycle. Early input and native overlay generation were
contradicted.

## Deterministic compound route and clean pair

`tools/sf2_mission1_route.py` latches all five movie identities before using a
compound TITLE predicate: retail application depth/state/transition 2/4/0,
TITLE word zero, enabled 320x240x15 display, and neutral PAD stable for 60 guest
frames. It then uses retail Cross presses to select New Game, One Player, and
leave state 8. State-0 success additionally requires a live player instance,
camera ownership, positive health, completed movie read, recorded PAD input,
and authoritative matrix movement.

The first two native routes passed semantically, but later input landed on
different guest frames because the host socket issued `press` after polling a
gate. Same-frame fingerprints matched through TITLE and diverged after the
jittered input. This was a harness invariant, not accepted determinism.

The generic debug command now accepts `at_frame`. The emulation-thread
consumption point records first/last applied frame, value, and count. Four
bounded harness failures were retained during refinement: endpoint readiness,
a Python predicate binding error, live polling missing a short pulse, and frame
history being ordered before override application. None was a retail failure.
`debug_input_schedule` protects the final contract; commit `89804a7` also adds
the route comparator. The complete suite passes 43/43.

Two final clean native-enabled processes used identical scheduled intervals:
Cross 19200--19219, Cross 19320--19339, Cross 24000--24019, and D-pad Up
25800--25859. Both reached state 0 with the same player/camera owner and ended
at XYZ `(-5606,2036,7529)`, health 150, armor 600. Both had zero lost CD INT1,
nonzero GPU work, more than 1,200 SPU key-ons, and identical XA totals.

The normalized comparison passed. Startup hash is
`e044f13241a622bd02b465e9e68270c2753976b0f5a40f2beb607596ac8b32ce`;
input hash is
`c518cd5e1e597e70eebc0e82e8b305dcc56f6499f8672a52867cdc89bdefd650`.
Exact intersecting-frame fingerprints match at TITLE, aircraft movie, state 8,
player ownership, and post-movement. These fingerprints include RAM writes,
store PCs, MMIO, scratchpad, and cycle clocks. Only host request timing and the
equivalent Y=0/Y=240 double-buffer bank are normalized.

Final ownership remains explicit. Run A/Run B report resident AOT
15,829,452/15,828,754; compiled overlay 141,709,302/141,708,765; interpreter
fallback 683,327/683,189. Fallback is 0.4799%/0.4798% of overlay-tier dispatch
and is not called native coverage. Both runs have eight loaded regions and 573
registered candidates at player control. The ignored footprint is 6.604 GiB.

The full report is `../OVERNIGHT_REPORT_2026-08-03.md`; the payload-free corpus
return is `../PSX_PORTS_RETURN_2026-08-03.md`.

## 2026-08-03 human connected slice and final-SIO recorder

The user completed a visible OpenGL cold-boot route through all of Mission 1,
one death/checkpoint reload, mission completion, retail save, and the beginning
of Mission 2. The ignored recording contains exactly 43,967 input-only records
and the memory card was written. This is accepted as human functional evidence.

The initial deterministic replay gate failed honestly. A software/headless
control consumed the file but did not reproduce the OpenGL route or card. A
hidden-OpenGL replay exposed the decisive boundary error: frame 20,552 required
PAD `0xFFEF`, while final SIO state was `0xFFFF`. `sdl_vblank_present()` sampled
host input a second time after pacing, after the recorder/replayer had run.
Thus the artifact was valid but represented the pre-pacer sample rather than
what retail consumed.

The title-neutral correction gives replay exclusive ownership over the late
sample, records after the final sample and mod hooks, finalizes every early-
return path, and makes replay exhaustion/mismatch sticky. `pad_timeline_test`
and `pad_timeline_final_sample` cover the contract. The rebuilt two-process
preflight passed with 320 samples, an exact 20-frame pulse at 240--259, and a
replay observation at frame 242. The first recording is retained but rejected
as deterministic evidence; one corrected human recording and two clean hidden-
OpenGL replays remain. See `../PAD_TIMELINE_REPORT_2026-08-03.md`.

## 2026-08-03 isolated modernization pass 1

The user explicitly authorized a separate modernization branch after accepting
the compatibility checkpoint. `experiment/sf2-modernization-pass1` branches
from `2009297`; the compatibility branch and executable remain untouched.

Read-only comparison with SF1 and the other SF2 stream established two safe
boundaries: enable the framework's existing 4x OpenGL supersampling without
game code, and terminate mouse/keyboard actions at retail PAD semantics.
Game-specific camera addresses and direct state writes were rejected.

The new generic mouse adapter maps buttons and bounded relative movement into
the active-low PAD word. Horizontal movement produces retail turn pulses;
vertical movement is gated by retail L1/manual aim. Configuration parsing,
SDL2/SDL3 relative-mode compatibility, focused unit coverage, and isolated
build/launch profiles were added. The modern profile is 4x OpenGL, borderless
desktop 4:3, nearest filtering, and no interpolation or widescreen.

Validation retained three harness defects. First, the new Release build
inherited `PSX_DEBUG_TOOLS=OFF`, so no route endpoint existed; the build now
enables validation tools explicitly. Second, selecting a 600-frame boundary
from a host-polled TITLE sample produced two successful but nonmatching input
schedules. The route now derives all four inputs from the retained retail TITLE
movie event; `sf2_mission1_route_schedule` covers it. Third, a hidden run
entered app state 7 with unsolicited `0xFFEF` D-pad Up. Unfocused keyboard
sampling was fixed generically, then a second neutral probe identified the
remaining source as dev-any-input's intentional background-pad merge. The
modern launcher now uses strict assigned-device routing for its lifetime. A
clean hidden probe then reported `0xFFFF` and centered sticks.

Final runs F and G passed authentic startup, TITLE, retail New Game/One Player,
aircraft FMV, state 8, player ownership, and movement. The strict comparison
passed with matching startup/input hashes and normalized fingerprints at all
five checkpoints. Final XYZ is `(-5606,2036,7529)` in both. Ownership is
resident AOT 15,830,035/15,829,527, compiled overlay
143,524,201/143,523,764, and interpreter fallback 683,265/683,155
(0.4738%/0.4737% of overlay-tier dispatch). Both runs have zero lost CD INT1
events, 1,208 SPU key-ons, and identical nonzero XA totals. Full handoff:
`../MODERNIZATION_PASS1.md`.

The first user-controlled modernization pass requested three small input
corrections. A separate aim threshold now makes RMB/manual aim three times more
responsive (4 counts versus 12), vertical motion is translated non-inverted,
and the hardcoded Tab turbo-present shortcut was deleted so Tab is exclusively
the configured retail R1/target action. Focused adapter, focus, and final-PAD
tests pass and the modern executable was rebuilt. Per user direction, further
feel iterations use focused automated checks followed directly by visible user
acceptance rather than repeating the complete seven-minute route each time.

## 2026-08-03 — modernization pass 2: direct mouse boundary proof

Created `experiment/sf2-modernization-pass2` from compatibility checkpoint
`5b64d86`; the frozen compatibility branch and executable remain available for
4:3 A/B reproduction. The private corpus and both read-only SF2 presentation
oracles were consulted before implementation. Their `0x80053464` and
`0x800539D0` addresses remained leads until checked against the local
SCUS-94451 input (SHA-256
`75A360BF7465DFDEC85C14F9BA93862AAE2531B48D83FD8D82BA8C9FFFA13D33`).

Local static proof found retail words `0x8EA30034` and `0x8FA20010`
respectively, both as resident AOT block leaders. A clean bounded Mission 1
route then recorded 32 hits at `0x80053464` after state-0 control. Its semantic
checkpoint independently proved state 0, live player/camera ownership and
authoritative movement. The initial attempt accidentally left more than one
route driver attached after a shell timeout; that evidence was rejected, every
owned process was stopped, and the proof was repeated with one runtime and one
driver under `lab/sf2/local/pass2-mouse-proof-clean/`.

Implemented a generic, configured direct-relative-mouse bridge. The recompiler
emits its callback only at the configured resident leader and rejects a word
mismatch during generation. The runtime rejects changed code, non-gameplay
state, invalid RAM pointers and scripted camera ownership. Mouse buttons remain
ordinary PAD actions; motion bypasses PAD and receives separate chase/aim yaw
and pitch sensitivities with non-inverted Y by default. A bounded debug seam
feeds the same accumulator for deterministic validation. Focused commands:

```powershell
cmake --build recompiler\build-cli --parallel
gcc runtime\tests\test_mouse_camera.c runtime\src\mouse_camera.c `
  -Iruntime\include -O2 -Wall -Wextra -Werror `
  -o lab\sf2\local\mouse-camera-test.exe -lm
lab\sf2\local\mouse-camera-test.exe
powershell -ExecutionPolicy Bypass -File `
  tools\build_sf2_modernized_pass2.ps1
```

The recompiler build, focused unit test, full SCUS-94451 regeneration and
isolated pass-2 link succeeded. No generated retail source was hand-edited.

The first live direct-input probe found valid state/player/wrapper/base
pointers but no applied delta. The one-vblank lifetime was too narrow for
SF2's nested callback cadence; a bounded four-callback lifetime retained input
until the semantic block and still discarded it across scripted ownership.
Chase `(40,20)` then applied as yaw/pitch `(30,20)`. Held retail L1 plus
`(-24,-12)` applied once on the aim path as `(-24,-12)`. A live bounded watch
also observed 16 executions of `0x800539D0` under L1, narrowing that external
lead without adding a duplicate hook.

Native-wide was enabled with the generic GTE gameplay classifier after a clean
state-0 sample measured 2,497 projected vertices while TITLE, FMV and briefing
measured zero. Automatic screen-cull discovery produced zero calls with its
default 224/241-line signature and again after adding the locally observed
240-line signature. Both hypotheses are recorded as contradicted; no address,
movie geometry or presentation dimensions were hardcoded to manufacture a
result.

The first hidden-software attempt was rejected because a short-timeout shell
left one route driver alive and a second driver observed its scheduled Cross
press during the neutral-TITLE hold. All owned processes were stopped. The
single-driver repeat passed under
`lab/sf2/local/pass2-hidden-software-b/route.json`. It reports native-4:3 for
TITLE, the depth-24 aircraft movie and state 8, then a 512x240 native-wide
surface (`nw_extra=128`) at state-0 control and authoritative movement.

The clean hidden-OpenGL acceptance route under
`lab/sf2/local/pass2-hidden-opengl-a/route.json` passed the same retail gates
and the newly automated chase/aim checks. GL final-present evidence records a
centered 1440x1080 destination for TITLE/state8 and coherent depth-24 CPU movie
scanout, then the full 1920x1080 destination from the wide FBO during gameplay.
The native-wide debug report was corrected to stop labeling mode 2 as
unconfigured merely because its legacy squash ratio remains identity.

`python -m py_compile tools/sf2_mission1_route.py`, the direct mouse unit, the
Release regeneration/link and the complete registered 48/48 suite pass.
`git diff --check` passes. The ignored footprint is 7.721 GiB. Campaign culling,
scope/NVG/fade/matte behavior and mouse feel across Missions 1--8 remain human
acceptance gates; the frozen 4:3 launcher remains available for A/B.

The objective audit after checkpoint `65e3c49` rejected treating the Mission 1
renderer gate as campaign-wide proof. Existing rings establish wide versus 4:3
ownership, but did not say whether an authored fade/filter rectangle actually
covered the revealed margins. Generic cumulative `fullscreen_rect` checks and
expanded counters were added to `gpu_state`; no SF2 address or effect identity
is encoded.

`sf2_disc1_validation_monitor.py` drains the bounded present rings at two-second
intervals, detects sequence gaps, records only state/classification transitions
and ten-second semantic samples, and finalizes exact executable/config/BIOS/
settings and PAD-timeline identities after endpoint closure. It captures no
pixels, guest payload, RAM/audio, movie data or overlay bodies and never writes
guest state. The companion launcher creates blank isolated cards and records a
natural Missions 1--8 run. A separate frozen-baseline A/B launcher copies card
files out of a closed enhanced session into a new writable directory rather
than sharing or modifying evidence.

The focused monitor model verifies ring de-duplication, missed-sequence
accounting, transition compression, GL target classification and PAD structure.
An invisible OpenGL/dummy-audio smoke ran 483 frames, drained both rings,
quit cleanly and finalized a structurally valid 483-sample timeline plus all
four configuration hashes. All three PowerShell launchers parse, full pass-2
regeneration/link succeeds, and the registered suite is 49/49. Human Disc 1
acceptance remains open; see `../DISC1_VALIDATION.md`.

## 2026-08-03 — native-wide finite-backdrop ownership closure

Mission 1 native-wide rendering exposed correct extra terrain and coherent HUD
but a finite outdoor sky/SCRIM mesh ended at the authored 4:3 boundary. GP0
census grouped draw submissions by consumed ordering-table rank: the first
textured rank contained the background polygons, while later ranks owned
terrain, actors, particles and HUD.

Three production hypotheses were falsified. Global far-depth GTE expansion
filled the margin while destroying world/character projection. Bounded exact
GTE callers either left the gap or changed foreground geometry. Raw palette and
packet-source gates isolated the sky but were rejected as payload identities.
The retained generic rule is opt-in and frame-local: latch the first OT rank
that submits textured polygons and stretch only textured polygons from that
rank in the native-wide mirror. Rank `0xFFFF` cannot establish ownership. A
focused unit covers discovery, same-rank matching, nontextured/unknown rejection
and frame reset.

Clean OpenGL and hidden-window software routes passed the complete authentic
Mission 1 gate. Exact software live A/B proved the broad software polygon-colour
seams are unchanged by the owner toggle; only the black reveal wedge changes.
That precision issue remains separate and open.

The final framework rebuild changed the overlay cache hash. An interpreter-only
control passed, honestly measuring 15,841,809 resident-AOT and 13,410,471
fallback dispatches. Reusing older broad captures lost the debug endpoint while
leaving state 8, so those shards were discarded. The five images captured by
the successful final executable were compiled under its exact codegen/config
hash; seven unsupported isolated fragments remained fallback. A new blank-card
OpenGL run passed through state-0 movement and semantic chase/aim with
15,831,907 resident-AOT, 6,862,443 compiled-overlay and 542,932 interpreter
dispatches (7.3316% of the overlay tier), 25 image loads, zero lost CD INT1,
1,210 SPU key-ons and final XYZ `(-5606,2036,7529)`.

Registered tests pass 49/49 and `ws_backdrop_owner_test` passes directly. The
exact Disc 1 candidate hashes are recorded in `../DISC1_VALIDATION.md`. Human
Missions 1--8 acceptance is the next gate.

## 2026-08-03 — human parachute scene contradicts textured-rank stretching

The first human Disc 1 run exposed a deterministic visual divergence in the
Mission 1 parachute opening. The canonical 4:3 centre remained correct, while
both 16:9 reveal margins contained enlarged and discontinuous environment
polygons. Two user screenshots establish the same failure during the scripted
opening and player-owned parachute gameplay.

The bounded GP0 capture resolves the ownership error. The first textured OT
rank is a connected set of projected `0x3C`/`0x34` environment polygons with
coordinates extending beyond the authored display, not an independent flat
2D image. Applying the 4:3-to-16:9 ratio to each polygon therefore transforms
real curved environment geometry a second time. The fast wide path then copies
the authoritative canonical centre over that mirror, making the bad transform
visible specifically in the margins.

Corpus classification was updated as follows: semantic SCRIM/background
ownership remains narrowed but open; first-textured-rank ownership is
contradicted; global/exact GTE callers remain contradicted; raw palette/source
identity remains diagnostic-only; and the hybrid project's coherent world-OT
compensation is relevant behavior but not directly transferable to this
renderer. The production profile disables `nw_phase_backdrop`. This removes
the proven corruption and deliberately reopens the smaller finite-edge reveal
instead of containing it with a presentation approximation.

The screenshots also exposed a separate generic fullscreen-effect defect:
cinematic top/bottom mattes covered the authored centre but not the reveal
margins. `ws_expand_fullscreen_rect` required both complete width and complete
height, which can recognize fades but cannot recognize a matte, letterbox band
or partial-height scope mask. The replacement semantic predicate expands an
axis-aligned rectangle or quad when it spans both authored horizontal edges;
height is intentionally irrelevant. Mono rectangles/quads, Gouraud quads and
textured/Gouraud-textured quads share the same pure helper. Projected curved
geometry cannot match its axis-aligned topology.

The focused `ws_fullwidth_effect` regression covers 4:3 identity, exact-width
and reverse-winding partial-height mattes, narrow rejection and projected-quad
rejection. It passes under strict C99 warnings, and the registered suite passes
49/49 with `PYTHONUTF8=1`. Candidate executable
`98CC6480F423505F1BA5C42481992E24CABC2F41173E8789270F11279A499441`
is rebuilt for human parachute/matte validation; no visual acceptance is
claimed yet.

The first hidden-window software acceptance attempt reached authentic state 0
with native-wide active (`384x240` authored display, `512`-pixel wide mirror),
but reported 65,148 fullscreen checks and zero expansions. This is the exact
first semantic divergence, not a rendered-frame inference. The remaining bug
was coordinate space: SF2's gameplay packets author the horizontal display as
`-192..+192` and GP0 draw offset `+192` maps it into framebuffer `0..384`.
The initial predicate compared the pre-offset packet coordinates directly with
`0..384`, so no genuine matte could match.

The title-neutral helper now accepts the authored horizontal origin explicitly.
GPU call sites derive it from `draw_area_left - draw_offset_x`, expand in the
packet's own coordinate space, and suppress subsequent HUD re-anchoring for a
recognized full-width effect. The regression now includes centered-origin
rectangles and quads in addition to zero-origin, reverse-winding, narrow and
projected cases. Strict C99 compilation, Release regeneration/link and the
registered 49/49 suite pass. The resulting executable is
`6D618BC788D33E40ED57EC42D97658CC4BC6A5B28AB0EA30779B8FC7BF2F2568`.
A fresh hidden-window software route passed the complete authentic gate:
stable TITLE at frame 18,616, aircraft FMV at 19,580, state 8 at 23,888,
state-0 player ownership at 25,411 and verified movement at 25,904. Widescreen
remained inactive with zero effect checks through TITLE/FMV/state 8, then
reported 147,255 checks and 7,035 expansions at player ownership. Evidence is
under ignored `lab/sf2/local/pass2-fullwidth-origin-sw-20260803-192740/`.
The matching hidden OpenGL route also passed: stable TITLE at frame 18,603,
aircraft FMV at 19,573, state 8 at 23,896, state-0 player ownership at 25,399
and movement at 25,899. It likewise records zero checks/expansions through the
4:3-owned phases and exactly 7,035 expansions at player ownership. Evidence is
under ignored `lab/sf2/local/pass2-fullwidth-origin-gl-20260803-193605/`.
Human visual acceptance is still pending.

## 2026-08-03 — enforceable Disc 1 candidate identity

The completion audit found that `DISC1_VALIDATION.md` still named the candidate
preceding the backdrop rejection and origin-aware fullscreen fix. The actual
automated-pass executable and generated config are respectively
`6D618BC788D33E40ED57EC42D97658CC4BC6A5B28AB0EA30779B8FC7BF2F2568`
and `96DFC6A4FE1E036838E2D3274546832B21014F588690EBAFA100D4E6B9D2A180`.

`lab/sf2/modernization/pass2-candidate.json` now records those identities plus
settings, OpenBIOS and the frozen 4:3 executable. The recorded Disc 1 launcher
hashes all five files and refuses to open a runtime on any mismatch. A focused
source regression keeps the manifest, human guide and launcher synchronized;
it and a direct local five-file verification pass. This records no retail
payload and does not modify either build.

## 2026-08-03 — human rejection of split native-wide rasterization

Human Mission 1 screenshots reject the automated native-wide candidate. The
canonical 4:3 centre is coherent, while both reveal margins contain strongly
fan-shaped affine-textured world polygons and visible discontinuities exactly
at the old 4:3 edges. This direct presentation evidence overrides the earlier
semantic-route acceptance. It is not classified as ordinary missing culling:
geometry exists in the margins, but its raster continuity is wrong.

The OpenGL fast path renders only margin-reaching primitives into the wide FBO,
then copies the canonical high-resolution framebuffer into its centre at
present. Software instead mirrors every primitive across one full wide surface.
The first bounded A/B sets `nw_full_mirror = true`, disabling the OpenGL centre
blit and centre-only primitive skips. The enhanced build was regenerated, but
the candidate manifest was deliberately not advanced: Mission 1 human visual
acceptance is required before this becomes a milestone. No PGXP, retail-code
change, address rule, or game-state containment was introduced.

The missing-side-geometry hypothesis was also checked against the generic
screen-extent detector. SF2 displays gameplay at 384x240, making width
immediates `0x180/0x181` a bounded lead. It is contradicted for this detector:
all captured overlay variants contain zero `slti/sltiu` sites with either
immediate, and an independent scan of the user-owned resident executable also
finds zero. Seven resident `slti 0xF0` height comparisons exist, but there is no
paired 384-wide signature. The production config therefore remains unchanged;
the zero automatic-cull count is not fixed by substituting display dimensions.
The hybrid project's broader room/portal envelope remains an architectural
lead only and requires a locally verified SCUS-94451 traversal owner.

## 2026-08-03 — native-wide edge-culling candidate

Human testing confirms the full-mirror A/B corrected the split-raster margin
distortion. It also identifies the next first divergence: world geometry is
culled near the new 16:9 edges. Read-only comparison with the hybrid project's
verified Mission 1–4 correction narrowed ownership to guest GTE horizontal
projection, not a 384-pixel retail screen-limit immediate.

The title-neutral opt-in applies native-wide aspect scale to the X value visible
to guest code and records unsquashed retail X at the precise GTE projection
site. GP0 restores retail X only when the command word's guest RAM address and
packed value exactly match recorded SWC2 provenance. Retail visibility thus
sees the wider cone without compressing geometry on the already-wide raster;
CPU-authored HUD/effects remain in their original coordinate space. Menus,
FMVs and other native-4:3 phases remain outside the active predicate. No PGXP,
SF2 address rule, generated-code edit or forced state was introduced.

The focused recompiler config regression and `gte_register_access_test` pass,
and the Release build links. Candidate executable SHA-256 is
`8555B60512EDC0CC266D340439D713C7E3DD8946C1B7994FBC5CEFB5569DC64F`;
generated config SHA-256 is
`89E65A8646565CC6418577629B1FDE5FC92D38D11444B9BAFDFE2E0B3E16045A`.
The Disc 1 manifest remains unchanged pending human visual acceptance.

The first human run of the guest-projection candidate still produced a black
rectangular region at one reveal edge. It did not erase the scene top-to-bottom:
foreground terrain remained while the outdoor sky/SCRIM ended on a stable
rectangular boundary, and holding the camera could grow the border square.
This narrows guest GTE projection to necessary-but-insufficient for this frame;
the remaining symptom is the already-open finite background coverage owner,
not evidence that every world model is still rejected.

The next bounded A/B enables the existing title-neutral textured-edge rule.
Unlike the rejected first-textured-rank transform, it leaves every canonical
and interior vertex unchanged and expands only textured polygon vertices that
retail already placed outside the authored 4:3 boundary. Aspect-derived scale
is used; full-mirror and exact guest-projection provenance remain enabled.
Focused parser tests pass. Candidate executable SHA-256 is
`6A9C14777FCE96EC163DD31378549BFFE913922EB9F41FABF0A17C6CE798EF16` and
config SHA-256 is
`9B38FB1632EAA23A78CB30D71442F05E9F153D269BA2D8F3C2A959BCC4AF2FB9`.
The recorded Disc 1 manifest is still unchanged pending visual acceptance.

Human evidence rejects this edge-only A/B as well: the rectangular black
SCRIM boundary remained, while Gabe appeared horizontally thinner. The latter
is consistent with incomplete unsquashed-X provenance leaving some packet paths
under the guest projection scale; visual parity cannot be claimed from the
aggregate restore counter. Guest projection is therefore narrowed but not
production-safe, and textured-edge expansion is contradicted for the observed
finite boundary.

The generated profile has been returned to the last accepted presentation
combination: native-wide full mirror enabled, guest projection and textured-edge
expansion disabled. This retains the known finite SCRIM edge rather than
manufacturing coverage or risking mixed character aspect. Rebuilt executable
SHA-256 is
`84D62E39AA201FAE881127353CE4C66CFCC5E8800C41B3C1108CBF234C4A2295` and
config SHA-256 is
`06D2C40300CE7AC015A5E5C52D06B726463EB781485A87F61DF6D846A87DB2DF`.

The next human observation separates the finite backdrop from general retail
visibility: NPCs also disappear while visibly inside the added side field, but
the floor remains. This is consistent with resident floor submission plus
4:3-frustum actor/background rejection. The guest projection is therefore
reinstated, but its presentation inverse no longer depends on partial SWC2
address provenance.

The generic linked-list prepass now runs for this mode and identifies the
frontmost eligible HUD ordering-table rank. Every polygon in earlier world
ranks is inversely compensated once around the live authored display centre;
the HUD rank, direct GP0 submissions and axis-aligned full-width effects remain
unchanged. This mirrors the hybrid oracle's world-OT/presentation split without
copying an address or classifying by texture payload. Release regeneration and
the focused parser tests pass. Candidate executable SHA-256 is
`136285C525E835E7E470D0BF5A01DDAF5FA847544F0246E737EEAF177DED139A` and
config SHA-256 is
`89E65A8646565CC6418577629B1FDE5FC92D38D11444B9BAFDFE2E0B3E16045A`.

Human testing rejects this third candidate. Edge culling remains and Gabe's
aspect is wrong again. This contradicts world-ordering-table inverse
compensation as a complete projection composition and ends the bounded visual
A/B sequence. Together with the preceding exact-provenance and textured-edge
tests, three distinct corpus-derived hypotheses have now been falsified or
narrowed without containment.

The exact open divergence is submission ownership: NPCs and finite outdoor
background/SCRIM geometry disappear while still inside the widened display,
whereas resident floor geometry continues to render. No current evidence shows
that OpenGL clipping or the full-mirror presenter owns that loss. The next
probe must capture the transition at the retail model/room visibility layer
and correlate it with the bounded GPU packet census. The generated production
profile is restored to `nw_full_mirror = true` with guest projection and
textured-edge transforms disabled; this preserves correct character aspect and
the known culling defect rather than hiding it.

Release regeneration succeeded and the focused `recompiler_patch_test` passes.
The restored executable SHA-256 is
`F47B337D391BA44CE57B436E5B739CB27DE47A3EC8E2E7DA67AE29544D5E586C`;
the generated config SHA-256 is
`06D2C40300CE7AC015A5E5C52D06B726463EB781485A87F61DF6D846A87DB2DF`.
The executable identity differs from the earlier full-mirror build because it
retains inactive diagnostic code, while the active config identity is the same.

For the next falsifiable check,
`tools/start_sf2_widescreen_cull_capture.ps1` launches this restored profile
with an isolated card directory and arms the existing bounded GPU census. At a
human-confirmed disappearance boundary it dumps only the preceding 360 frames,
GPU state, application/player identity and input hashes under `lab/sf2/local/`.
It does not write guest RAM, force state, capture pixels, or terminate the
runtime. Python and PowerShell syntax checks pass.

## 2026-08-04 — rhythmic triangle flicker and GTE value provenance

The user's stationary-camera screenshots and live observation established a
rhythmic individual-face defect after the broad ordering-table projection
inverse. A live `gl_wide_fast` A/B retained the flicker while switching away
from the independent wide FBO. A live 4:3 guest-projection A/B also retained
the defect while changing aspect/FOV. Both toggles were restored afterward.
Bad-frame packet captures cover both display pages; the repeating 13-command
SCRIM cycle correlates with the cadence, but its CPU VRAM copy co-simulation is
exact.

The broad OT ownership rule was removed. `GeomVertex` now retains the exact
pre-squash X and exposes it through a value-provenance lookup independently of
optional high-precision rendering. GPU polygon execution requires all three
packed positions to match before composing the retail X delta. Quad halves use
separate draw coordinates, so no matched half mutates the other. The guest
projection setter also now keeps exact precision tracking enabled while native-
wide projection is active.

Direct focused builds pass for `test_ws_projection_compose.c`,
`test_gte_register_access.cpp`, and `test_gpu_primitive_reject.c`.
`recompiler_patch_test` and its C++17-header companion pass. The isolated
Release executable links successfully. Two clean hidden OpenGL routes pass all
five retail semantic checkpoints and finish at identical player XYZ. Their
normalized fingerprints match, while host-timed movie/input records differ and
are recorded as non-deterministic rather than normalized away.

The build script gained an explicit build-name parameter and copies the current
modern cache/capture inputs into isolated candidates. That check exposed stale
overlay emitter namespaces rather than native coverage: current emitter
`d6bc536d` cannot load old `9713afe3`/`dae7adf8` DLLs. A current single-image
preflight passes; the broader seven-region preflight builds 127 shards but
fails four audits. The failing set was not published. Candidate route ownership
therefore remains resident AOT plus interpreter fallback with zero compiled-
overlay dispatch, documented in
`docs/sf2/WIDESCREEN_FLICKER_PROVENANCE_2026-08-04.md`.

Ignored footprint after the work is 13.034 GiB, below the 20 GiB ceiling.

### Rejection and DMA-submission ownership correction

Human testing decisively rejected the value-provenance candidate. It produced
severe rhythmic face corruption and changed character proportions. A live
read-only `gpu` query measured 1,156,788 projection restores, rising to
1,475,298 shortly afterward; the supposed narrow match was therefore broad.
Changing only the live GTE aspect to retail 4:3 stopped the severe corruption
and restored proportions while immediately restoring the known edge cull.
This separates the presentation regression from the original visibility defect.

The rejected inference was equality of packed SXY values. Those values are not
stable primitive identity: unrelated geometry and UI can reuse them, and the
cache lifetime crossed submissions and frames. Exact value equality therefore
cannot recover ownership after DMA ordering has been discarded.

A bounded three-second census at the restored culling position recorded 45,061
draw commands over 61 retail draw frames in
`lab/sf2/local/widescreen-cull-live-20260804-restore-ab/`. A simultaneous DMA
trace and read-only traversal of current linked lists identified six submissions
per gameplay tick. The first three contain no draws; the fourth contains 696
polygon draws in the sampled scene; the remaining lists contain 31 auxiliary
draws and 10 sprites. This independently matches the hybrid oracle's rule that
only the first actual world submission receives projection compensation.

The next candidate therefore retains guest-visible wide GTE projection but
restores X uniformly only inside a dense polygon-owned linked-list submission.
It does not use packed-coordinate provenance, an SF2 address, packet payload,
or ordering-table rank. The census now records linked-list ordinal/root so this
ownership can be falsified directly. A focused title-neutral regression covers
the dense-submission predicate at, below, and disabled thresholds.
## 2026-08-04 — native-wide acceptance checkpoint

- Human A/B testing accepted the linked-list submission candidate: correct
  proportions and wide margins, no observed actor/sky culling, black-edge flash,
  or rhythmic triangle-face corruption in the exercised Mission 1 scenes.
- The packed-SXY value cache was removed. It was a disproved ownership model:
  coordinate equality crossed frames/submissions and caused more than 1.15
  million false restores.
- The accepted owner is semantic submission structure. Bounded evidence shows
  three setup-only linked lists, one dense approximately 696-polygon world list,
  then 31-draw auxiliary and 10-sprite UI lists per representative tick.
- The generic runtime counts linked-list polygon commands and performs inverse
  projection only when the title profile's minimum is met. The threshold is
  explicit config (`nw_world_min_polygons = 64`), is disabled at zero, and
  contains no guest address.
- Source checkpoint: `a2b951c`. Rebuilt ignored Release executable SHA-256:
  `1A6B0DE5FDB4CCC7DE2D6AD99BAE89A878741B508114978D16465C12DDF1C529`.
- Validation: CLI Release package and candidate regeneration/link pass; 50/50
  registered tests pass with `PYTHONUTF8=1`; focused projection-composition and
  primitive-size regressions pass under `-Wall -Wextra -Werror`.
- Corpus consultation for the next pass confirms `PSX-GPU-005` as the PGXP
  contract and `PSX-TIME-002` as the high-refresh warning. VSync-divisor changes
  are not a valid 60 FPS implementation. PGXP comes first; presentation
  interpolation precedes any title-sensitive simulation unlock.
- The payload-free corpus return was committed and pushed as `6fd986a`. Only
  `_knowledge/projects/sf2-recomp.md` and the dated modernization return were
  staged. The portfolio-wide validator remains red because of an unrelated
  pre-existing retail BIOS artifact; it was neither read nor changed.

## 2026-08-04 — PGXP pass-1 acceptance

The first isolated PGXP pass implements the private corpus's `PSX-GPU-005`
contract without copying a title address. Precise GTE X/Y/depth follows exact
SWC2 RAM address and generation into a renderer-neutral triangle sidecar. GPU
assembly applies it only after a complete three-vertex match; all incomplete
ownership fails atomically to native integer coordinates and affine texture
mapping. Canonical guest-visible VRAM remains affine. OpenGL consumes precise
positions and reciprocal depth through its vertex stream; software consumes
the same metadata only in high-resolution/presentation mirrors. Bounded debug
counters report complete, partial, unmatched, CPU-authored, stale, address,
packed-value, and invalid classes.

Human A/B testing accepted the corrected OpenGL candidate as visually excellent:
culling is absent, aspect remains correct, and polygon wobble/texture swimming
are materially reduced. The first candidate had accidentally omitted the
accepted `-GuestProjection` build switch; exact generated-config comparison
found `nw_guest_projection = false`, and a config-only R2 rebuild corrected it.

Gate D/E qualification ran only after that acceptance. CLI Release generation,
link, 50/50 registered tests, and the focused canonical-VRAM regression pass.
Two clean hidden-window OpenGL routes pass authentic startup through retail
Mission 1 state-0 movement and compare with zero errors: identical startup and
input hashes, matching normalized fingerprints at all five checkpoints, and
identical final player/camera state and XYZ. A clean software route passes the
same semantic gates and matches all normalized fingerprints; its polling loop
first observed the legal movie 19 frames later, so exact cross-renderer movie
observation time is not claimed.

The untouched frozen 4:3 OpenGL executable also passes a bounded hidden-window
launch with widescreen unconfigured and inactive. The accepted compatibility
artifact was not rebuilt or overwritten.

An initial qualification launch used `--headless`; that mode intentionally
omits the renderer and therefore reported widescreen unconfigured before
stalling at state 8. The owned process was stopped and the route was rerun with
`--hidden-window`, the correct invisible renderer mode. This tooling failure is
retained as a rejected qualification attempt.

Final OpenGL telemetry records 390,433 triangles, 14,823 complete exact PGXP
matches, and 375,610 unmatched, with zero partial/stale/address/packed/invalid
samples. Coverage is therefore selective and residual in-motion instability is
not disproved. Overlay ownership is approximately 15.99M resident-AOT, zero
compiled-overlay, and 13.53M interpreter fallback because the copied cache hash
is stale. The ignored footprint is 14.836 GiB. Full payload-free record:
`docs/sf2/PGXP_PASS1.md`.

## 2026-08-04 — high-refresh prerequisite: current-hash overlay diagnosis

The high-refresh branch began from accepted PGXP source checkpoint `2eebc41`.
No presentation-interpolation code was added because honest current-hash
compiled-overlay execution first exposed a deterministic semantic regression.

Additive history recovery now unions the latest capture, immutable directory
fragments, and the legacy single-file `.d` layout. The runtime performs a
recoverable file-to-directory migration before publishing a new contribution.
The diagnostic build migrated its 19,900-byte legacy file to
`overlay_captures.json.d.legacy-FF8D85BAF6CCFAAC.json` and retained it while
creating the additive directory. Focused migration, ingestion, malformed-input,
crash-ring, and whitespace checks pass.

The generated project uses its copied `psxrecomp/runtime`, not the repository
root directly. A fresh isolated build was therefore configured after packaging
and copying the current framework. Ordinary atexit reports now include the
always-on native-call ring. The valid cache namespace is
`cg9_82e77d3e_gcd6e97ca6`; it contains 384 ABI-`0x15` DLLs. A clean route
registered 443 candidates across 12 regions, reached about 145.6M native
overlay calls with about 97.8K interpreter fallbacks, and had no invalidation,
stale-block, or overflow signal.

That route nevertheless exits reproducibly at guest frame 24003 during the
scheduled state-8 briefing exit. The all-interpreter control reaches state-0
movement. The first native report named `0x8000293C` in progress. Blocking only
that entry moved the marker to sibling `0x80002954` without changing the exit.
Blocking every captured entry into their shared OpenBIOS RAM exception/callback
body `0x80002814..0x800029CC` removed the in-progress marker but still exited at
the same frame while retaining about 142.9M native calls. These controls reject
the final call/body as the root cause and require an earlier candidate-set
bisection.

One family-control attempt was invalidated when concurrent manual debug queries
displaced the single active route client; its monitor raised `MemoryError`.
The clean repeat used no parallel debug client and reproduced the frame-24003
exit. No runtime process remains. Resume with `PSX_NATIVE_RANK_LIMIT` bisection,
then an exact block control at the first changing rank; do not retain any
blocklist as containment. Full handoff: `docs/sf2/HIGH_REFRESH_PASS1.md`.

During source-sync diagnosis the ignored accepted PGXP executable was
accidentally rebuilt. The source branch and accepted config are unchanged, and
the build was reconstructed from detached `2eebc41`, but the Windows link did
not reproduce the historical PE hash. The reconstructed control hashes
`DE284A5BBBF7C783CC68A90C97937CF8BB9B1AD6B780581178B83E51794C95F2`.
All subsequent work is isolated in `build-high-refresh-pass1-diag` (hash
`85072BD189D0F844F4D0B3C732C523EB7AC61B582164127936E59F6025B03205`).

## 2026-08-05 — exact native owner and range-scoped resident gate

First-live native-rank bisection isolated the deterministic frame-24003 exit
to rank 306, `0x8001DA48/58690F42`, from shard
`0001C000_7A4D33C6.dll`. Rank 305 passes and rank 306 fails; blocking only this
entry while leaving every other candidate native passes. A shadow run entered
the candidate but did not return and produced no valid diff session, so it is
retained as a bounded diagnostic-tool failure rather than semantic evidence.

The mandatory corpus check identified `PSX-OVL-005` as the closest established
class. SCUS-94451 comparison found a JALR-to-NOP at `0x8001DA9C` inside the
candidate's exact `0x5BC` range, plus an unrelated data-word change at
`0x8001C754` outside it. This explains the earlier zero-match result: a
whole-capture classifier rejected the mixed image even though the emitted
function was a pure resident CFG-presence variant.

`compile_overlays.py` now compares configured resident text per exact guarded
function identity. Any identity whose guarded changes only insert/remove
control-flow words publishes an atomic `unpromoted` sidecar. The loader omits
marked shards during Windows/POSIX scans and checks again immediately before
loading. Captures and compiled artifacts remain available for audit; the
interpreter retains authority for only the unsafe native shard. A focused
regression includes the SF2-shaped mixed capture and marker lifecycle.

Two clean hidden OpenGL Mission 1 routes pass state-0 movement and compare with
zero errors. They reproduce startup/input hashes, all checkpoint fingerprints,
card hashes, camera/player ownership and final XYZ `(-5606,2036,7529)`. The
repeat records 145,821,779 native overlay dispatches and 147,947 interpreter
fallbacks; the full native ring contains no `0x8001DA48`. Focused CTest is 5/5,
`git diff --check` passes, and the ignored footprint is 17.43 GiB. Evidence and
the exact bisection table are in `docs/sf2/HIGH_REFRESH_PASS1.md`. The final
diagnostic executable hashes to
`59557D6D1640FFEE236504E5EBC16E5C12AFE1ADA2D1EC37A9834EDDB3E6C0BB`.

## 2026-08-05 — first 60 Hz presentation candidate

The isolated R1 config enables the existing OpenGL presentation interpolator
at 60 Hz with VSync off while leaving retail simulation and VBlank timing
unchanged. The config parser now accepts the runtime-supported 60 Hz floor and
rejects 59 Hz. Mission-route checkpoints now include `gl_interp` telemetry.

The first launch was invalid because the new release tree had
`PSX_DEBUG_TOOLS=OFF`; the game ran but the route endpoint could not listen.
Reconfiguration with observation tools enabled fixed the validation build.
The next clean route passed and measured approximately 19.9 authentic
captures/s feeding exactly 60.00 presentations/s in gameplay. Interpolation
remained suspended with zero swaps through TITLE, FMV and state 8.

Comparison initially reported only a LEGAL startup-frame mismatch. Mandatory
corpus reconsultation found no matching established lesson. Diagnosis showed
the route requested one filtered CD history entry even though the endpoint is
newest-first, making the claimed first frame host-poll dependent. It now
requests 64 bounded matches and chooses the oldest `(frame, seq)`; the focused
route regression passes.

The clean repeat passes Mission 1 and compares with zero errors against the
frozen native route. Startup/input hashes and all semantic/fingerprint checks
match; final XYZ is `(-5606,2036,7529)`, health 150, armor 600. Interpolation
has two history frames after player ownership, ends with 1,882 swaps, and loses
zero CD INT1. Evidence is
`evidence/high-refresh-r1-route-repeat-20260805-042525`; exact executable hash
is `0199B159814B6E6FD047AC8993FEAFAB3D2C1F7990629A2F95AD63A282C9B851`.
The user candidate is `Launch SF2 High Refresh R1.bat`. Full qualification,
commit and push remain deferred pending visual feedback.

## 2026-08-05 — R1 visual rejection

User testing rejected the first 60 Hz presentation candidate: motion felt the
same as the accepted build and visible motion ghosting appeared. This overrides
the green automated cadence/semantic gate for release qualification.

Inspection confirms that linear interpolation crossfades two full render
targets without motion estimation. The available motion-adaptive mode only
thresholds that blend to a previous/current pixel choice; it cannot generate
spatially correct intermediate geometry and is expected to exchange ghosting
for stepped or shimmering moving regions. No cosmetic R2 from this family will
be sent to the user.

The candidate-only 60 Hz parser relaxation and its tests were reverted. The R1
shortcut now refuses to launch and explains its rejected status. Ignored binary,
config and evidence are retained for audit. The route's bounded interpolation
telemetry and corrected oldest-sector-history logic remain. Future high-refresh
work requires renderer-side spatial interpolation/reprojection with proven
provenance and HUD/effect exclusions; retail simulation remains unchanged.

## 2026-08-05 — spatial high-refresh R2 handoff gate

Renderer-side replay now reconstructs retail world presentation from recorded
GP0 primitives rather than blending whole frames. The identity probe established
that consecutive updates use disjoint packet addresses, so the conservative
matcher binds the dense retail list and combines immutable textured material
with mutual-nearest geometry. Ambiguous and untextured packets fail closed.

Several bounded alpha=0.5 routes initially produced zero changed pixels. Their
rejection counters showed material/state candidates but no in-bound geometry.
SF2 alternates VRAM draw pages: recorded positions already contain the E5 draw
offset. Normalizing by that offset before matching and rebasing after
interpolation produced approximately 305,473 matches, including 258,420 moving
matches. Validation also had to inspect the next draw page rather than the
current scanout page. The corrected route has 21/21 pixel-exact alpha=1 samples
with zero diff and 9,695,239 changed intermediate pixels.

Output queues the spatial half frame until retail displays its matching VRAM
page, then presents a discrete half/authentic pair with no temporal crossfade.
It requires two exact replay samples; otherwise presentation remains authentic.
Two clean-process output routes passed:
`evidence/high-refresh-spatial-output-20260805-065333` and
`evidence/high-refresh-spatial-output-repeat-20260805-070114`. The repeat ends
with 592 output pairs and 1,787 swaps; TITLE, FMV and state 8 remain at zero
spatial frames/pairs/swaps. Retail player ownership, movement and route success
remain intact.

The handoff executable is `SCUS94451_HighRefreshSpatialR2.exe`, SHA-256
`BE274DAA80605B452E4991E8CA43C9069B084CFFE9E9ABDEA77DBE328E4ABC97`.
The exact shortcut is `Launch SF2 Spatial High Refresh R2.bat`, with separate
R2 memory cards. Focused spatial tests pass 7/7; full qualification, commit and
push are deferred pending the required visual feedback.

## 2026-08-05 — spatial R2 rejected and reverted

The user observed non-steady gameplay speed, including substantial speed-up,
and widespread texture cracking/seams. R2 is rejected. Its launcher now refuses
to start, and spatial capture/matching/replay/output plus its debug/probe wiring
were removed from active source. Accepted PGXP, native-wide rendering, direct
mouselook, retail timing and unrelated compatibility fixes were preserved.

The failure closes two validation gaps. A deterministic route with matching
endpoint guest state does not establish guest progression per wall-clock second
under visible presentation, and alpha=1 pixel-exact replay does not establish
watertight alpha=0.5 geometry. Independently matched adjacent triangles can use
different motion histories and separate along shared edges. Whole-frame blend
and packet-nearest-neighbor replay are now both rejected high-refresh families.

Future work must compare directly with the SF1 authoritative snapshot/transform
architecture and independently locate the corresponding SF2 ownership seam.
Before another handoff it needs bounded wall-clock speed, shared-edge coherence,
and title/FMV/fade/HUD exclusion gates.

## 2026-08-05 — transform snapshots, flash rejection, and R3 handoff

Exact GTE RTPS/RTPT provenance and semantic object-space MinHash matching now
support a single-thread render-at-will prototype. Capture-only and output routes
proved ~61 Hz guest VBlank, ~19 Hz retail world submission, exact alpha=1
reconstruction, and zero shared-edge midpoint mismatches. The first visible
output nevertheless produced user-reported major white flashes. Automated
success was not treated as acceptance.

Bounded diagnostics identified two unsafe ownership errors. Projection had
been applied per vertex, allowing a triangle to move partially when another
vertex failed interpolation; replay now commits an entire primitive or snaps
it. More importantly, one latest snapshot cannot represent SF2's alternating
VRAM display pages at Y=0 and Y=240. R3 retains independent immutable base,
raw-texture, command, and metadata snapshots for both pages and selects the
exact displayed page. A diagnostic initially aliased Y=240 into slot zero by
splitting 512-line VRAM at 256; the verified slot rule now preserves 0 and 240
separately.

The corrected route `high-refresh-transform-twopage-fix-20260805-132105`
passed: guest 60.98 Hz, world 18.99 Hz, display 60.48 Hz, 1,881 transform
presents from 629 captures, page slots `[[1,0,0],[1,0,240]]`, one expected
startup miss, zero render failures, exact parity 2/2, 408 moved midpoint
vertices, 320 exact repeated vertices, zero midpoint mismatches, and zero
overflows. The executable SHA-256 is
`8494883760FCA00AA3876643697C946961B564A9ADC7FC764A02E2991CAE9EE8`.
The isolated handoff is `Launch SF2 Transform High Refresh R3.bat` with
separate R3 cards. Stop for the user's visual verdict before qualification.
