# PS1B-220 load-delay work

Exposure: the handover records prior PS1B-102 reference-behavior comments.
Implementation inputs are PSX-SPX, the clean L1 specification, and its authored fixtures.
No restricted reference source is used.

The baseline is b8ccd50fafda9b90fb5cd765ec62318aff663027.
Run `python runtime/tests/test_load_delay_l1.py` with WinLibs UCRT gcc on PATH.
The fixture executes the production decoder through a test-only entry point.
Unexpected device or dispatch seams terminate the test.

The first baseline test uses L1 case a at a game-RAM address.
It fails at O0 with `slot=bbbb0002 after=bbbb0002`.
L1 requires `slot=aaaa0001 after=bbbb0002`.
Evidence is `../ps1b-220-baseline-a.log` in the private workspace parent.
This establishes the eager-value failure on this decoder path only.
It does not establish native, overlay, precise, exception, or performance results.

The intended change carries pending values in CPUState across executor boundaries.
An instruction reads the old GPR, then its write can cancel the pending value.
An LWL/LWR merge reads the pending value without exposing it to ordinary operands.
Accepted exception entry commits the pending value before the handler reads registers.
The timing model remains outside this change.

Full L1 cross-executor comparison, save round trips, seven Tier 1 routes,
Tier 3, and an interleaved two-title performance comparison remain required.
The branch is private preparation and must not land before those gates.

## Candidate checks

The uncommitted candidate passes 32 decoder and save checks at O0 and O2.
The checks cover the ordinary load widths, repeated destinations, merge pairs,
cancellation, a fault in the successor, separate CPU states, and both pending ages.
The focused runner also executes five authored sequences through generated CFG C
and the production decoder at O0 and O2. These include a load in a branch slot.
Evidence: `../ps1b-220-cross2.log` and `runtime/tests/test_load_delay_l1.py`.
These are bounded instruction tests. They do not run source-profile device timing.
Both recompiler executables build with WinLibs UCRT gcc 16.1.0.
The full-function emitter, DLL transport, full 119-case L1 matrix and source-mode
interrupt execution still need executable coverage before this branch is ready.

The draft wire version is 11, overlay ABI is 25, and codegen version is 12.
The integrator must reconcile these numbers with the final minted pin.
The draft checks pending state at each emitted instruction. Its hot-path cost is
unmeasured and must satisfy the two-title performance gate before landing.

The first full-function generated-C branch-slot case passes O0/O2.
Its fixture emits an authored ROM image and executes the generated C directly.
Evidence: `../ps1b-220-cross-full.log` and the runner's `--bios-emitter` option.
Five focused existing tests also pass: full-function emission, cross-page slots,
LWL/LWR emission, CPS emission, and CPU wire validation.
The CPU_EXEC section round-trip test passes O0/O2 after reserving its former load cells.
The current session does not open prohibited source. The exposure statement above
records the inherited handover warning, not a new source exposure.

A rebuilt baseline CFG compiler at b8ccd50fa fails the expanded native fixture:
check 39 returns BBBB0002 where the repeated-load slot requires AAAA0001.
The test uses the same current harness and runtime headers with the baseline emitter.
Evidence: `../ps1b-220-native-baseline.log`, baseline build logs, and `_scratch/baseline-source`.
The candidate passes seven CFG programs and two full-function programs at O0/O2.
These add branch-and-link behavior and the relocated link address.
Evidence: `../ps1b-220-cross4.log`.

Consulted PSX-SPX CPU Specifications again for exception and link ordering:
https://psx-spx.consoledev.net/cpuspecifications/ (2026-09-26).
The imported corpus has no current PSX-CPU-002 note. The failure catalog points to
that historical identifier, so it is a lead rather than an available authority.
The native trap helpers retain pre-existing fatal BREAK/overflow behavior.
Full rule-8 exception qualification must distinguish that executor behavior from
load-value retirement. No claim of full exception support follows from these checks.

### 2026-09-27: complete clean-fixture ingestion and conditional decoder replay
All119 authored RAM hashes and stock guest-log hashes match the clean L1 receipts; the fixture is now bound in runtime/tests/load_delay_l1_clean.json. Actual RAM program base is0x1000. The supplied human listing tool prints0x500-based addresses even though its RAM hash matches the0x1000 image; use rebuilt RAM/receipt identity, not listing address labels.

runtime/tests/test_load_delay_l1_matrix.py runs all119 complete guest programs through the real decoder and GTE module at O0/O2 and matches all20 output words per case (ps1b-220-matrix5.log). IRQ EPC and Cause are replayed from the oracle as input stimuli, and SYSCALL is routed through the real interpreter exception-entry helper by a test seam. Handler instructions execute through the decoder. This is conditional CPU-value evidence, not timer recognition, real IRQ transport, native/overlay or full precise-slice qualification. Those gates remain open. First attempts exposed fixture seams (missing module name, hook signature, missing syscall route, Cause stimulus), not new production failures.

### 2026-09-27: all 23 non-IRQ native L1 programs
The same 23 complete non-IRQ programs now pass at O0/O2 through actual generated CFG functions and the generated production dispatch table (ps1b-220-native-matrix-all1.log). No interpreter fallback is permitted by this runner. Exact byte output matches all20 clean oracle words. Input code/addresses are unchanged; native code-range validity and optional HLE/debug hooks are explicit fixture seams. The syscall row uses the previously declared exception-transport seam. The96 timer cases are still decoder boundary replays only. Overlay DLL and actual precise/timer paths remain unqualified.

Focused review found the existing savestate status protocol guard still demanded v10 after this branch's v11 bump. The old guard failed; it now checks v11 and passes. Updated the matching boot_state.h read-floor comment. No extra format change.

### 2026-09-27: actual overlay DLL transport
The authored two-function overlay now passes O0/O2 through the production loader and generated DLL: a load in the JR slot survives the loader return, and the next DLL function observes the old value followed by the loaded value. A host-armed pending load also survives DLL entry. The baseline emitter fails the first pending-state assertion with the same headers and loader (../ps1b-220-overlay-baseline.log); the candidate passes (../ps1b-220-overlay3.log). Run runtime/tests/test_load_delay_overlay.py --recompiler _scratch/build-recompiler/psxrecomp-game.exe. Timing, IRQ, and memory access callbacks remain authored seams. This is a bounded transport check, not all119 overlay fixture qualification.

### 2026-09-27: precise-slice entry for 23 non-IRQ programs
All23 non-IRQ clean programs pass O0/O2 through actual psx_slice_block_impl and psx_run_precise (../ps1b-220-precise4.log). The side-effect entry requests one-instruction slices; production ownership retains pending loads until a safe return. Dirty-page/dispatchability, device deadlines and timing remain explicit seams in load_delay_precise_seams.c.in. This qualifies slice continuation value state, not timer recognition. Attempting to reuse the outer-loop oracle IRQ injection fails g-irq-target04 because the slice can legitimately retain ownership across that EPC (../ps1b-220-precise-irq1.log). That injector is unsuitable inside a multi-instruction slice; no production defect follows. The96 cases still need an internal real timer/IRQ route. Earlier precise harness attempts exposed unbound data globals and dirty-page callbacks, now explicit.
2026-09-27 A1 result: PS1B-222 S1/S2 plus fast/source controls pass O0/O2 atop542730c72 (ps1b-222-spu-a1.log). All3 protected-file patches pass git apply --check there. PS1B-220 complete authored/no-BIOS runtime rebuilt after rebase (ps1b-220-runtime-a1-build.log); this closes build feasibility, not full L1 qualification.

### 2026-09-27: all119 L1 programs on complete production runtime
The authored RAM driver now links the complete rebuilt runtime on542730c72 plus220 and executes all119 L1 cases through dirty_ram_dispatch. Every case exits0 and all20 words match the clean oracle, including all96 timer cases (ps1b-220-l1-actual-all.log, _scratch/l1-actual-all/receipt.json). Timer MMIO, clock, source instruction boundaries, IRQ recognition/delivery and handler execution are real production paths; oracle EPC/Cause are comparison outputs only, never input stimuli in this driver. BIOS routing is an authored RAM dispatcher and test termination uses the production instruction-observer callback after the guest marker; no retail BIOS or source-emulator code is used. This result is the full runtime dirty executor at Release build optimization, not yet native/overlay/precise or O0/O2 qualification.
The old shared IRQ helper omitted source_gpu_runtime_init and therefore reached unsupported ROM continuation on this no-BIOS build. The new RAM-only L1 runner explicitly initializes the source route and avoids that unrelated fixture setup defect.

The actual precise-slice route also matches all119 cases after honoring the production slice gate's return0 as a request to continue through the dispatcher (ps1b-220-l1-actual-precise2.log). First attempt118/119 failed only SYSCALL because the authored dispatcher omitted that fallback at nested precise entry; production code unchanged. The same complete runtime now qualifies timer recognition inside the slice. Native dispatch plus precise source takeover is in progress; its first timer case passes, whereas disabling precise slicing leaves that timer undelivered (expected scope: no claim that default nonprecise native timing is qualified).
