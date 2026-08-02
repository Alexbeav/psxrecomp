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
