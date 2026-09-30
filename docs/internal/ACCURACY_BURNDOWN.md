# PSX Accuracy Burndown (living doc)

Companion to FAITHFUL_TIMING_PLAN.md. That doc owns the CYCLE/TIMING axis (the
area under active implementation); THIS doc is the full-coverage burndown across
ALL accuracy axes for the faithful-core build.

## ⚠️ LESSON (2026-06-26): oracle-validate OUTPUT before applying a fix

The MDEC "fix" (agent branch `fix/mdec-faithful-accuracy`) was applied from
research-claimed source-level divergences and REGRESSED both Tomba 2 FMVs
(Whoopee Camp logo + Tomba intro) on color — REVERTED. The original MDEC output
was user-verified CORRECT. Takeaways, now binding:
- A research-claimed discrepancy is a HYPOTHESIS, not a bug. Validate the OUTPUT
  (diff our decoded result vs Beetle's on the SAME input) BEFORE changing code.
- "Matches Beetle's source conventions" does NOT justify rewriting code whose
  OUTPUT is already correct. Our impl may be a valid equivalent.
- The user's eyes / the oracle's OUTPUT override source-reading. "Looked right
  before, wrong after" = revert, always.
- MDEC fix is PARKED on its branch pending a real MDEC output-diff harness (decode
  the same stream on native + Beetle, compare blocks). Only fix proven output
  divergence. Do NOT re-apply blind.
- Same gate applies to ALL agent fixes (hybrid-pad etc.): reconcile onto wt/tomba2
  + validate vs the oracle BEFORE trusting.

Separately: the Tomba intro FMV left/right SPLIT seam is a PRE-EXISTING GPU/display
presentation bug (present before today's work), not MDEC — its own axis-5 item.

## Method (non-negotiable)

- Every item gets: **status**, the **external comparative(s)** to cross-reference
  it against, and a **validation method**. "Looks good" is NOT a status — an item
  is only GREEN once cross-referenced against a reference (psx-spx / Beetle source
  / DuckStation / a hardware test ROM) AND validated against the oracle at runtime.
  Self-agreement (compiled == our interp) proves backend-equivalence, NOT
  correctness — both can be identically wrong (CLAUDE.md §15).
- Don't do it all in one pass. Tomba 2 is the **stomping ground**: validate
  everything we can here, then validate the rest against the other games
  (Tomba 1, MMX6, Ape, BIOS), then merge wt/tomba2 → master, then keep a
  **post-merge burndown** for whatever remains.
- Governed by CLAUDE.md Rule -1 (faithful core, no hacks, breaking other titles OK).

## Comparative sources (the reference shelf)

- **nocash psx-spx** — the canonical hardware reference. Cite the section per item.
- **Beetle/Mednafen PSX** (IN-TREE: `psxrecomp/beetle-psx/mednafen/psx/`) — our
  oracle's own source: cpu.cpp, gte.cpp, gpu.cpp, spu.cpp, cdrom.cpp, dma.cpp,
  timer.cpp, frontio.cpp/`input/`, mdec.cpp, sio.cpp, spu/reverb. Read for the
  exact model; run as the oracle on port 4382 for runtime diff.
- **DuckStation** source — clean, heavily-commented; excellent cross-check for GTE
  cycle table, GPU rasterization rules, CD timing (use as a 2nd opinion vs Beetle).
- **Hardware test ROMs** (ground truth above emulators): Amidog CPU & GTE tests,
  PeterLemon/PSX, the psx-spx test suite, GPU/timing test ROMs. Running these on
  native vs Beetle is the strongest single validation we can add.
- **Real HW via Beetle oracle** (port 4382) — first-divergence on the relevant
  state surface (VRAM for GPU, audio samples for SPU, cycle counts for timing).

## Validation infrastructure to BUILD (prerequisite tooling)

- [ ] **Native↔Beetle cycle/first-divergence comparator** (replaces stale
  DuckStation-era find_divergence.py port 4371). Needs additive guest-cycle
  exposure in beetle_debug_server.c. The backbone measure for axes 2/3.
- [x] **`PSX_FORCE_INTERP=1` restored (2026-09-29).** It marks pages dirty, and the
  native-safety checks decide flagged pages by their bytes, so clean game text kept
  running compiled. Both checks now refuse game text while it is set. Found taking
  the segment probe's interpreter column (SEGMENT_AWARE_CODE.md §7.2).
- [ ] **State-surface diffs**: VRAM byte diff (GPU), SPU sample-stream diff
  (audio), CD sector/response diff. Per-axis oracle comparators.
- [ ] **Hardware-test-ROM harness**: run Amidog/GTE/CPU test ROMs on native and
  Beetle, diff pass/fail + result registers. (Highest-leverage axis-1/2 validator.)

---

## Axis 1 — Instruction semantics (decoder)

Status: STRONG (recompiler core; proven byte-identical to interp on many funcs).
- [ ] ALU/shift/logical/sign-extension — cross-ref Amidog CPU test ROM.
- [ ] LWL/LWR/SWL/SWR unaligned — psx-spx "CPU Load/Store"; Amidog.
- [ ] MULT/MULTU/DIV/DIVU → HI/LO, div-by-zero & overflow results — psx-spx; Amidog.
- [ ] Overflow-trapping ADD/ADDI/SUB (vs ADDU/etc.) — do we trap? psx-spx "Exceptions".
- [ ] **Load-delay slot hazard** — KNOWN SIMPLIFICATION: interp loads land
  immediately (no 1-instruction delay). Cross-ref psx-spx "CPU pipeline"; Beetle
  LDAbsorb. Decide: model it or document why safe.
- [ ] Branch-delay slot (incl. branch-likely absence on R3000) — done; spot-check.
- [ ] COP0 (mtc0/mfc0/rfe, Status/Cause/EPC/BadVaddr) — psx-spx "COP0".
- [ ] GTE/COP2 math: fixed-point, saturation, FLAG register, all 30+ ops —
  cross-ref Amidog **GTE test ROM** (the definitive validator) + DuckStation gte.cpp.

## Axis 2 — Cycle/timing  ← ACTIVE (see FAITHFUL_TIMING_PLAN.md)

Status: Stage-1 (1 cycle/insn, single-source seam in place); Stage-2 in progress.
Oracle model fully transcribed in `CYCLE_MODEL_BEETLE.md`. Game-independent
BIOS-kernel ruler BUILT (region [0x80001C5C→0x80001CA4]); per-block-leader cycle
observe added to the recompiler so ANY block leader is anchorable on both backends.
- [x] Single-source `psx_instr_base_cycles` seam (identity), both backends.
- [x] cyc_watch dispatcher+prologue double-fire dedupe (was corrupting native Δ).
- [x] Per-block-leader observe (native now samples like Beetle, not only fn-entry).
- [x] **Load double-count FIXED**: Stage-2 #1a put +2 data-access in
  psx_instr_base_cycles while memory.c already charged +6/main-RAM-load → counted
  twice. Reverted opcode fn to pure execute base; memory.c is the single address-
  keyed owner. Ruler [c5c→ca4]: native 34→30 (exact), no FMV regression.
- [x] **Memory wait-state — DONE (full R3000A load model).** load=4 LANDED (the boot
  wedge was a real device-timing bug the accurate load exposed — pad ACK→IRQ7 made
  guest-cycle-paced, see axis 5; commit d8c4a8e). ReadFudge + LDAbsorb give-back state
  machine SHIPPED across both emitters + interp (psx_cyc.h §1/deps/DO_LDS; commits
  d8c4a8e/fade560/d597797) — load2 10→11. **Device-region MMIO read waits DONE** (commit
  9ae534d, branch wt/tomba2-mmio-waits): Beetle MemRW table in psx_cyc_readmem, size-aware
  (SPU +36/+16, CDC +6×size, GPU/MDEC/SysCtrl/FIO/SIO/IRQ/DMA/Timers +1, RAM +3, else +0).
  Ruler #2: all 15 loops (incl. mmio_timer +3, mmio_spu +38) == Beetle EXACT.
  RESIDUAL: DMACycleSteal (dynamic per-read DMA bus-steal, libretro.cpp:868) unmodeled.
- [x] **Mult/div completion-stall — DONE, validated EXACT in BOTH emitters**
  (commits a3e8f28 game, 180b821 BIOS). CPUState.muldiv_ts_done set by
  MULT/MULTU/DIV/DIVU, MFLO/MFHI stall to it (psx_cycles.c). Per-instruction
  charging default on this branch (both emitters). Ruler #2 (game): div +38,
  div_spaced +38 (absorb), mult +15 — ALL == Beetle. Ruler #1 (BIOS kernel):
  [c5c→ca4] native 30→56 == Beetle 56, STEADY DELTA 0 EXACT. FMV no regression.
- [x] **Instruction-fetch / I-cache — DONE both backends.** Stage 1 (interp, commit
  958a928) + Stage 2 (BOTH static emitters at cache-line leaders via the runtime PC;
  relocate_ra for BIOS shell/kernel; default-on; commit 0edb935). Faithful direct-mapped
  4 KB/256-line model (psx_icache.c, from Beetle ReadInstruction: +0 hit / +4 KSEG1 /
  +3+refill miss). Ruler #2 icache_miss == Beetle; ruler #1 [c5c→ca4] steady 56==Beetle
  AND native now produces the cold-refill spikes (77/84) that were absent. FMV no-reg.
- [x] **GTE per-command cycles — DONE, validated EXACT** (commit ec1fd76).
  CPUState.gte_ts_done armed in gte_execute (cost-1 table verified from beetle
  gte.cpp op returns; AVSZ4=5 not 6); any COP2 reg access stalls to it; both
  emitters + interp. Ruler #2 gte_rtps +15 / gte_nclip +8 == Beetle. FMV no-reg.
- [x] **Mult/div stall — also completed to the dirty-RAM interpreter** (commit
  75d5d1a): backend parity (interp was charging 0 for mult/div). By-construction
  + no-regression (no interp-path ruler yet — see below).
- [x] Instruction-fetch / I-cache timing — DONE (see above; commits 958a928 + 0edb935).
  The ruler's 56→84 cold spread (I-cache line-refill transient) now reproduced natively.
- [x] **Uncached (KSEG1) fetch is charged per instruction in both emitters (2026-09-29).**
  Beetle ReadInstruction charges +4 and clears the load give-back on EVERY fetch at
  0xA0000000 and above. The interp fetches at every PC, but both emitters charged only
  at line leaders. In OpenBIOS, 5,477 of 9,592 in-place ROM (KSEG1) instruction sites
  were uncharged: 21,908 cycles short per pass through that code. Both emitters now
  emit a fetch before every instruction whose runtime PC is uncached
  (`psx_fetch_uncached`, psx_instr_cost.h, shared with psx_icache.c). The A0/B0/C0
  call-vector stubs charge one fetch per executed word. Cached code keeps the leader
  rule, and KSEG0 game output is byte-identical. Tests: ctest `uncached_fetch_charge`
  (compiled == interp fetch path == Beetle transcription, per instruction, on
  OpenBIOS) and `uncached_fetch_codegen_test` (game emitter). Validated against live
  Beetle: see the land gate below.
- [x] **BIOS emitter tests cache-line starts on the runtime PC (2026-09-29).** The
  line-leader test used the ROM address, assuming every copy window preserves
  bits[3:0]. OpenBIOS copies its kernel from ROM 0x1FC1E4D4 to RAM 0x500, so every
  kernel line crossing was charged one instruction early (a hit) and the real
  crossing went uncharged: 1,152 kernel instruction sites differed from the interp.
  Retail profiles are unaffected (SCPH-1001's windows are 16-byte aligned;
  SCPH-101/5552 declare none). Covered by ctest `uncached_fetch_charge` (cached runs
  compared from a cold cache).
- [x] **Land gate for the two 2026-09-29 fixes above: live-Beetle validation (done
  2026-09-29).** SEGMENT_AWARE_CODE.md §7.2's two runs against psx-beetle, each with
  the same BIOS image on both sides: OpenBIOS (SHA-1
  95419841b5104d552b14810b1ecbe6c1358bcdf1) and the owner's own SCPH-1001 dump (SHA-1
  10155d8d6e6e832d6ea66db9bc098321fb5e8ebf). Native builds carry #418, so the mult/div
  axis cannot mask the result. (1) LLE boot (`bios_hle = false`) per-anchor parity to
  the shell: at the shell entry native − Beetle goes from −2,165,591 to −126 cycles on
  OpenBIOS and from −7,049,462 to −454 on SCPH-1001. The ROM boot path (reset, kernel
  copy, main) is exact on both. The residuals are the IsC gap (axis 4 below) and, on
  SCPH-1001 only, −9 from the kernel entry through its KSEG1 alias
  (SEGMENT_AWARE_CODE.md §3.3). (2) Ruler #1 [0x80001C5C→0x80001CA4] on
  SCPH-1001: native (master and the fix) equals Beetle on all 64 passes. Ruler #2
  (15 loops) stays exact, and Beetle confirms the OpenBIOS memcpy cost (39 cycles per
  byte in boot, 42 in R4 in game). Per-anchor numbers: FAITHFUL_TIMING_PLAN §5,
  2026-09-29.
- [ ] **HW test-ROM ruler (#2)** — Amidog CPU/GTE timing ROMs for hand-crafted
  single-COMPONENT isolation (div-only, load-only loops) that organic BIOS code
  can't give (the prologue combines div+loads in one block). Strongest validator.
- [ ] Validation: native cumulative cycles == Beetle == analytic on the rulers.

## Axis 3 — Interrupt / event timing

Status: PARTIAL.
- [ ] Device IRQ-raise timing (VBLANK scanline, timer overflow/target, DMA/CD
  completion) — tied to axis 2/5; psx-spx per device.
- [ ] **IRQ take-point** (HW = exact instruction; us = block edge) — the parked
  precise-slicing (PRECISE_IRQ_SLICE.md). Validate vs Beetle exc_ring.
- [ ] Exception entry record (EPC/Cause.ExcCode/BD/Status-stack) — currently uses
  a sentinel EPC; cross-ref psx-spx "Exceptions"; Beetle. Validate exc_ring match.
  - Compiled game `syscall` (confirmed 2026-09-29): the game emitter sets no
    `cpu->pc`, and under CPS it is 0 inside a body, so a syscall that reaches
    the BIOS handler records EPC 0. The emitter also ignores
    `psx_syscall`'s transfer result. Only Enter/ExitCriticalSection, handled
    directly, occur in R4. The fix changes every title's codegen, so it is
    its own change; see SEGMENT_AWARE_CODE.md §9.

## Axis 4 — Memory map / MMIO

Status: MODERATE-STRONG (regions games use).
- [ ] KUSEG/KSEG0/KSEG1 mirroring, scratchpad, cache-isolation (IsC) — psx-spx.
  Code segment (links, EPC, I-cache tags, KSEG1 fetch cost) is designed in
  docs/SEGMENT_AWARE_CODE.md (acceptance ledger `segment_aware_codegen`). Oracle
  note: Beetle tags the I-cache by full virtual address; hardware clears bit 31 of
  KSEG0 fetches first (cpu.c 719-730). psx_icache.c follows Beetle (owner decision,
  2026-09-29; SEGMENT_AWARE_CODE.md §10).
  - 2026-09-29, segment-aware PR C: a KUSEG-linked EXE compiles at its KUSEG link
    addresses (links, EPCs, fetch tags, store PCs), and static dispatch is keyed by
    the full PC, so an alias of static text runs interpreted with its own segment
    instead of through another segment's body. On the synthetic probe, compiled code
    now equals Beetle and the interpreter in every result word and cycle-watch
    interval on both BIOSes. Open: overlay segments (PR E).
  - 2026-09-29, segment-aware PR D: segment-qualified seeds compile per-segment
    variants of static code (the direct-edge closure, with its own names, rows and
    PCs; KSEG1 variants charge a fetch per instruction), and dispatch finds the
    exact PC among a word's rows. The probe's KSEG0 and KSEG1 `probe_run` runs are
    native now and still equal Beetle, with an empty segment-miss ring. BIOS
    variants too: SCPH-1001's KSEG1 kernel entry `0xA0000500` (below). Still
    normalized in the BIOS dispatch: every PC without a variant row runs the body
    of its window's segment, as before.
  - 2026-09-30, PR D review fixes: such a BIOS alias entry is no longer silent.
    It is recorded in the segment-miss ring as kind `bios` with the home PC. LLE
    boots of OpenBIOS and SCPH-1001 through the probe's disc boot record none, so
    `0xA0000500` is the only alias entry either BIOS makes there. Without its seed
    the ring names exactly `0xA0000500`. Open: whether an alias entry should run
    interpreted instead of through the home body; that changes behaviour and needs
    a measured case, and none is known. Also: the runtime still treats a PC in
    `0x20000000`-`0x7FFFFFFF` as game text by its physical address
    (`psx_game_address_in_text` masks), where Beetle's `addr_mask` does not fold
    it onto RAM. The recompiler now refuses such seeds; the runtime side is
    unmeasured.
- [ ] IsC stores do not reach the I-cache model. memory.c drops every store made while
  SR.IsC is set. Beetle's WriteMemory rewrites the tag and valid bits of the addressed
  line when the I-cache is enabled and BIU has a tag-test, invalidate or lock mode bit
  set; that is how FlushCache (A 44h) invalidates the cache. Natively the cached
  kernel handlers keep hitting after a flush where Beetle refills them. Measured in the
  axis-2 land gate: −42 cycles after each of three OpenBIOS flushes and −445 after four
  SCPH-1001 flushes, all before the shell. A local prototype of Beetle's write (a few lines in the three
  `psx_write_*_raw` IsC branches) makes both LLE boots match Beetle at every anchor to
  the shell, apart from SCPH-1001's −9 KSEG1 kernel entry. The fix is PR #435.
  2026-09-29: with segment-aware PR D (which closes the −9) and #435 merged
  locally, both LLE boots are at 0 at every hit of every anchor through the shell
  entry.
- [x] **SCPH-1001's KSEG1 kernel entry (2026-09-29, segment-aware PR D).** The
  reset code enters the relocated kernel through `0xA0000500`; Beetle charges the
  four trampoline instructions as uncached fetches (20 cycles), and the BIOS
  dispatch ran the body compiled for runtime `0x00000500` (11). A seed in
  SCPH-1001's profile now compiles a KSEG1 variant of it. LLE boot against Beetle:
  the first hit of every SCPH-1001 anchor is 0 (was −9); the shell entry is −445,
  the IsC gap alone. Guard: `bios_segment_variants` (static check of the seed).
- [ ] BIU bit 11 (I-cache disable, 0xFFFE0130) does not reach the fetch model:
  memory.c stores it, psx_icache.c and the interp ignore it. Beetle charges +4 per
  fetch while the cache is disabled (CPU_SetBIU). This matters only for RAM code run
  with the cache off; `psx_fetch_uncached` is an address test and does not cover it.
- [ ] **RAM 0x0-0xF boot scratch diverges from Beetle** (found 2026-09-29 while
  reviewing segment-aware PR B). At the segment probe's spin on SCPH-1001 (disc boot,
  LLE), Beetle's RAM `0x0`-`0xB` is `00000003 275A0C80 03400008`: the delay-loop
  scratch word and words 2-3 of the exception-vector stub the boot copies there.
  Native (before and after PR B) has zeros: game start runs
  `memory_clear_low_boot_scratch()`, and memory.c's RAM-0 store filters drop later
  stores (Beetle's write log shows the in-place ROM store `0xBFC0D634` writing word 0
  after game start). memory.c's comment says the stub copy is not visible on
  hardware; Beetle contradicts it. The filters were added for Tomba 2's card write
  with buffer 0. Decide them with oracle evidence from that title, then keep or drop
  them. PR B only re-keyed the filters and gated them to SCPH-1001's own
  instructions (SEGMENT_AWARE_CODE.md §9); it did not change what they do.
- [ ] I/O register semantics: read-to-clear, write-1-ack (I_STAT), masking,
  unmapped/garbage reads — psx-spx "I/O Map"; Beetle memory.cpp.

## Axis 5 — Peripherals / devices  ← SUSPECTED WEAKEST (user flag)

Status: MIXED — "works for tested games," NOT edge-validated. Likely more gaps
than we think; the **hybrid-pad failure in Tomba is an axis-5 (SIO/controller)
bug**, not timing.
- [~] **SIO / controllers / memcard**: DualShock config-mode handshake (0x43),
  analog vs digital pad ID, the **hybrid pad mode failure** (Tomba) — Beetle
  frontio/input + psx-spx "Controllers and Memory Cards". HIGH PRIORITY per user.
  - DONE 2026-06-27: **pad ACK→IRQ7 timing made guest-cycle-paced** (was
    access-count-paced via `sio_irq_countdown=SIO_IRQ_DELAY_PAD`; pad fast-path
    now arms the cycle-paced ack scheduler, BAUD+ACK=1258 cyc, like the card
    path). This was the cause of the load=4 Tomba 2 boot wedge (accurate CPU
    exposed access-paced pad timing). See WEDGE_load4_shell_rootcause.md.
  - DONE 2026-07-26: Tomba now offers explicit Analog / D-Pad modes; the
    Tomba-only legacy pad-config fork and its debug/config surface were removed.
    Every title now uses the modern DualShock config state machine.
  - TODO (axis5 Fix-6 / "1.0e-e2"): fully remove the pad fast-path so pad+card
    share the unified shifter path.
- [ ] **GPU**: GP0/GP1 command set, rasterization rules (top-left fill, dithering,
  semi-transparency modes, mask bit, texture windows, blending), VRAM-as-texture —
  cross-ref DuckStation gpu_*, Beetle gpu.cpp, GPU test ROMs; validate by VRAM diff.
- [ ] **SPU**: 24 voices, ADSR, pitch/sample-rate, reverb, volume sweeps, IRQ —
  Beetle spu.cpp; psx-spx "SPU"; validate by audio-sample diff.
- [ ] **CDROM**: command set, response timing, sector read (data/XA/CD-DA), seek,
  shell/lid — Beetle cdrom.cpp; psx-spx "CDROM"; validate by sector/response diff.
  - **KNOWN OK-TO-DIVERGE (we are MORE faithful than the oracle) — from PR #9:**
    Suppressed in `tools/devtrace_diff.py` (`KNOWN_DIVERGENCES["cdrom"]`; use
    `--strict` to see raw). Beetle stays the independent oracle (unpatched); the
    tool LABELS these deltas, it does not silence real ones.
    - **CD controller version (Test `19h`,`20h`)**: we return `94 09 19 C0` (real
      SCPH-1001 sub-CPU, 1994-09-19, per psx-spx "CDROM Test Commands"). Beetle
      hardcodes `97 01 10 C2` (`cdc.cpp:2253`, a later PSone board) regardless of
      BIOS. Version byte `< 95h` keeps shell CD-init flag `[0xA000DFFC]` clear;
      `>= 95h` sets it and forces a spurious boot ReadTOC. So native (94h) issues
      NO ReadTOC where Beetle (97h) does → expect a CDROM event delta at CD-init,
      possibly cascading briefly. Verified vs psx-spx, NOT the oracle. MMX6+Tomba
      soak PASSED (2026-07-06). Trivially revertable (4 bytes, `cdrom.c` Test 0x20).
    - **Status reg `0x1F801800` bit 2 (ADPBUSY)**: idles at 0 (was wrongly pinned
      set under an "ADPCM empty" label). psx-spx: bit2 = XA playback → 0 when idle.
    - **GetID license region**: derived from disc serial (`SCEA`/`SCEE`/`SCEI`)
      instead of hardcoded `SCEI`; NTSC-U (MMX6/Tomba) now correctly report `SCEA`.
    - Method reminder (top-of-doc LESSON): these were HYPOTHESES until OUTPUT
      validation — the MMX6/Tomba playtest soak WAS that validation. Revert any
      that regress a shipped title.
- [ ] **DMA**: all 7 channels, block/linked-list/chain modes, timing, DICR/DPCR —
  Beetle dma.cpp; psx-spx "DMA".
- [ ] **MDEC**: macroblock decode, IDCT, color conversion, RLE — Beetle mdec.cpp;
  validate FMV frame diff vs Beetle.
- [ ] **Timers (0/1/2)**: all clock sources (sysclk/dotclock/hblank/÷8), modes
  (target/overflow/reset/IRQ-repeat/one-shot), sync modes — Beetle timer.cpp;
  psx-spx "Timers".

## Axis 6 — Static-vs-dynamic fidelity (recompiler-unique)

Status: STRONG (most project effort lives here).
- [ ] Self-modifying / install-at-runtime code (dirty-RAM interp) — ongoing.
- [ ] Function discovery / dispatch completeness (no missed indirect/jump-table
  targets) — resolve all dispatch misses each run (Tomba2Recomp CLAUDE.md).
- [ ] Call/return contract + stack fidelity — the blue-screen/wedge class.
- [x] **Compiled BIOS hands the runtime its runtime PCs (2026-09-29, segment-aware
  PR B).** Relocated kernel and shell code stamped stores, and set syscall EPCs and
  fallthrough PCs, at their ROM addresses; the interpreter uses the executing PC.
  Both now agree (`runtime_pc()`, SEGMENT_AWARE_CODE.md §5.2). memory.c's seven
  SCPH-1001 store-PC keys moved to runtime PCs in the same change, gated so each
  matches only SCPH-1001's own instruction (not other BIOSes or game code at the
  same RAM address); the GP0 source key matches the same 5,596 boot commands as
  before. Guards: ledger `bios-runtime-pc`, `store-pc-keys-runtime`.
- [x] **KUSEG-linked EXEs compile for their link segment; dispatch is exact
  (2026-09-29, segment-aware PR C).** The EXE parser folded a KUSEG header
  (Alien Resurrection, Kula World) to KSEG0, so the game compiled for a segment it
  never runs in, a seed in its own segment was silently dropped, and the lookup
  ran a body for any segment's alias. Now the image compiles at its link segment,
  seeds and config sites are checked by physical address and segment (another
  segment's seed is a reported variant request), and a PC with no row of its own
  is a segment miss: interpreted through the clean-text-miss path and recorded
  (TCP `segment_misses`, `dispatch_stats`, exit report; SEGMENT_AWARE_CODE.md
  §5.3, §5.5). Kula World and Alien Resurrection were not run (no discs here);
  they need a regeneration with seeds, seeds directives and exact-match config
  sites in KUSEG (`tools/collect_game_misses.py --game-toml` now writes KUSEG
  seeds). Until PR E their overlay code runs interpreted: static code enters it at
  KUSEG PCs, which #417's gate keeps off the KSEG0-compiled shards.
- [x] **Call-contract return checks mask the segment (fixed 2026-09-29,
  segment-aware PR D).** `psx_call_contract` (`cpu_state.h`) and the four return
  checks of the generated BIOS dispatch loop compared `$ra`/`pc` with the call
  site's return PC physically, so a callee returning to an alias of its call site
  would continue in the caller's compiled body, where hardware runs the alias's
  segment. All five compare the full PC now (SEGMENT_AWARE_CODE.md §5.5). LLE
  boots and R4 fingerprints (12000 frames, cold and warm) are unchanged by it.
  Guard: ledger `exact-return-contract`. Masked PC compares left elsewhere are not
  return checks against a call's link: the overlay shadow run's return test
  (`overlay_loader.c`, PR E's scope) and the interrupt/pump "did the PC move"
  tests (`dirty_ram_same_pc`, `same_guest_pc`, savestate's resume match).
- [ ] Backend equivalence (compiled == interp) — necessary, not sufficient.
  Measured with `tools/fp_identity.py` (2026-09-29): seeded warm vs cold
  overlay-cache runs, judged on the `frame_fingerprint` guest-fact columns. R4,
  12000 frames on #417+#418+#420: IDENTICAL with 497 tolerated one-write VBlank
  straddles.

## Axis 7 — Determinism

Status: SOFT SPOT.
- [ ] Boot run-to-run variance observed (sometimes wedges, sometimes clean) —
  track down; faithfulness presupposes determinism.

---

## Research findings (2026-06-26, parallel subagents, cross-referenced vs in-tree Beetle + psx-spx)

Per-axis deep-dives in `accuracy/*.md`. Headlines + priorities below. NOTE much is
already faithful — these are the GAPS. Implement serially (one branch each, code to
just-before-build, pull in + validate on the oracle one at a time). NONE of these
overlap the cycle axis or each other (different files), so they parallelize.

- **`accuracy/axis5_sio_controller.md` — HYBRID PAD BUG ROOT-CAUSED.** P0: cmd `0x43`
  must transmit the LIVE button frame (like `0x42`) + use the trailing byte to toggle
  config; we send all-zeros → active-low → "all buttons pressed" phantom input every
  `0x43` frame. P0: `0x45` status must report the LIVE analog/digital mode, not always
  analog-ON (→ frame-length misparse after a flip). Both were fixed, and the
  Tomba legacy fork was deleted 2026-07-26 when Tomba moved to explicit
  Analog / D-Pad modes. P1: analog-mode lock (`0x44`); P2: `0x4D`
  rumble echo + cycle-paced pad ACK. Validate: `sio_trace` diff on `0x43`/`0x45`.
- **`accuracy/axis5_gpu.md`.** P1: dithering ENTIRELY MISSING in both renderers (decoded
  but never forwarded) — largest systematic banding divergence. P2: SW rasterizer uses
  float + inclusive spans vs HW fixed-point half-open DDA w/ top-left bias → shared-edge
  double-draw + 1px over-fill. P3: sprite FlipX/FlipY unmodeled; mono-rect size mask
  wrong. FAITHFUL: all 4 semi-transparency modes, mask set/check, texture-window, CLUT
  4/8/15, texel-0, ×2 modulation, GP0/GP1 coverage. Validate: VRAM byte-diff + GPU test ROMs.
- **`accuracy/axis5_spu.md` — weakest subsystem.** P0: reverb ENTIRELY MISSING; P0:
  volume sweeps missing + ~2× gain-scale error; P1: noise gen + pitch-modulation absent;
  P1/P2: no SPU IRQ address-match, no capture buffers, SPUSTAT hardcoded; P1: host-audio-
  pull timing not guest-768-cycle clock. FAITHFUL: ADPCM decode + ADSR envelope (verbatim
  Beetle ports). Recommend: port Beetle PS_SPU on a guest-clock timeline. Validate: audio
  sample-stream diff.
- **`accuracy/axis5_mdec.md` — explains the slightly-off FMV.** HIGH: IDCT rounding/clamp
  diverges (contrast/ringing); HIGH: YUV→RGB lacks HW green-precision truncation (off-hue
  greens); HIGH: dequant domain/bias/clamp wrong (DC banding). MED: 4bpp output broken
  (textures, not FMV); MED: one-shot decode vs FIFO/block state machine. Validate:
  per-block decode diff + live FMV pixel-diff.
- **`accuracy/axis4_memory_mmio.md`.** GOOD: I_STAT/I_MASK/DICR ack semantics CORRECT.
  P1: RAM mirror wrong (we gate `<2MB`; HW aliases 2MB DRAM across an 8MB window 4× —
  mirror accesses silently read 0 / drop writes). P2: `IsC` over-broadly drops scratchpad
  writes (scratchpad is the D-cache, must stay addressable). P3: level-IRQ relatch; P4:
  per-segment addr masking; P5: open-bus high bits on I_STAT readback. Validate: MMIO
  trace diff + RAM-mirror sentinel probe.

Cross-cutting: every validation is a native(4500)↔Beetle(4382) ring-buffer diff on the
relevant state surface — the same oracle methodology as the cycle axis. Several need the
test-ROM harness (axis 1/GPU) which is worth building early.

## Phasing

1. NOW: cycle axis (axis 2) Stage-2 on Tomba 2 (FAITHFUL_TIMING_PLAN.md).
2. Build the comparator/test-ROM tooling (enables GREEN-ing items above).
3. Burn down axes here on Tomba 2 where validatable; axis 5 (esp. SIO/controller
   for the hybrid-pad bug) is the priority second front.
4. Validate cross-title (Tomba 1, MMX6, Ape, BIOS) before merge.
5. Merge wt/tomba2 → master; keep this doc as the post-merge burndown for the rest.
