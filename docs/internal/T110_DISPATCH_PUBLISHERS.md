# T110 — Who publishes a PC the dispatcher later runs

Status: delivered with T13 on `eagle-claude/t13-bios-seeds` (2026-09-17).

## Why this exists

August builds stopped on unknown dispatches at BIOS ROM addresses that no seed
could fix: 11 mid-block instructions and 7 delay slots inside functions that
were already compiled (`0192C 02B7C 04830 048B0 048F0 04A9C 04BA4 04C18 04DCC
04F9C 0504C 05084 06660 07814 07974 08620 098DC 0A554`, all `0xBFC0xxxx`; see
`python tools/bios_seed_corpus.py classify`). Compiled code only publishes
branch targets, fall-throughs and call returns. Every other PC the dispatcher
runs is published by the runtime, and one of those sites handed out a PC the
static table cannot re-enter. Seeding such an address as a function start hides
the publisher, so the corpus no longer carries any of them (T13).

## The rule

A published PC must be re-enterable: a compiled function entry or registered
continuation (every block leader), dirty RAM the interpreter owns, or other
RAM owned by the game/overlay gates.

- `psx_is_dispatchable()` (traps.c) answers that exactly for BIOS-owned code.
  It uses the backend's new `is_entry()`, a lookup over the generated dispatch
  table. BIOS ROM and the clean relocated kernel window are dispatchable only at
  entries and continuations.
- BIOS ROM exists at compile time, so it is never interpreted (CLAUDE.md
  Rule 18). An interrupt that becomes deliverable at a ROM PC that is not
  re-enterable stays pending until the next re-enterable boundary. That is the
  same edge compiled code already takes it at.
- Every runtime publish goes into the always-on `publish_ring`
  (dispatch_publish.h) together with the gate's answer. An unknown-dispatch
  fatal names the matching publisher, or says that none matched, which means a
  generated body or the guest produced the PC.

## Audit table

Line numbers are from this branch. "Pass-through" means the site forwards a PC
that someone else published.

### Runtime publishers

| Site | Publishes | Finding | Disposition |
|---|---|---|---|
| traps.c `psx_is_dispatchable` | gate for every row below | Returned 1 for any non-zero, non-sentinel PC. Every "fail-loud" guard built on it did nothing. | **Fixed.** Exact for BIOS ROM and the kernel window through `psx_bios_is_entry`; dirty RAM stays dispatchable; other RAM unchanged. |
| interrupts.c exception entry: EPC selection | `EPC = g_dirty_safe_resume_pc ?: compiled latch ?: top-level cpu->pc` | ROM acceptance was gated on the stub, so a mid-block ROM PC became an architectural EPC, went into the TCB and was dispatched later. | **Fixed** by the real gate: a non-entry ROM PC takes the designed sentinel path instead. Ring sites `epc_real` / `epc_sentinel`. |
| interrupts.c exception entry: Cause | `Cause & ~0x7C` | Kept BD/BT/CE from the previous exception, so a compiled-boundary IRQ could carry a stale BD. | **Fixed:** `Cause & 0x0000FF00`, as Beetle `PS_CPU::Exception` does. |
| interrupts.c `psx_check_interrupts_delay_slot` | `EPC = branch` with BD/BT | Faithful, but a ROM branch is not re-enterable. | **Fixed:** deferred when the branch PC is not resumable (`irq_epc_resumable`). Ring site `epc_delay_slot`. |
| interrupts.c `psx_irq_arm_compiled_resume_pc` | resume latch for savestate / rewind / netplay / scheduler resume | Sticky forever: later bare `psx_check_interrupts()` calls delivered with a stale EPC. | **Fixed:** consumed by the first compiled boundary check. |
| interrupts.c deferred in-exception switch | `cpu->pc = g_exception_real_epc` | Pass-through of the entry EPC. | Gated by the fixed `psx_is_dispatchable`. Ring site `deferred_switch`. |
| traps.c `psx_scheduler_resume_at` | savestate / rewind / netplay / selfcheck resume | Pass-through; the savestate candidate list includes `$ra`. | Gate is real now: a non-dispatchable resume crashes loudly. Ring site `sched_resume_at`. |
| traps.c scheduler TCB dispatch | `run_pc = TCB EPC` | Pass-through of any guest-saved EPC. | Gate is real now. Ring site `sched_tcb`. |
| traps.c syscall RFE | `cpu->pc = TCB EPC` or `COP0 EPC` | Pass-through. | Ring site `rfe`. |
| dirty_ram_interp.c MTC0/CTC0 software interrupt (exec path, two sites) | `EPC = pc+4` | Wrong when the write sits in a delay slot: slot+4 skips a taken branch. | **Fixed:** EPC = the taken target (`cop0_write_resume_pc`), slot+4 only when not taken. Ring site `cop0_swi`. |
| dirty_ram_interp.c MTC0 after a straight-line instruction | `EPC = next_pc ?: pc+4` | Correct: `insn` is never a slot here. | Ring site `cop0_swi`. |
| dirty_ram_interp.c precise slicer, IRQ take | `EPC = committed pc` | In the source profile ROM is admitted, so a ROM EPC could land at any instruction. | **Fixed:** `precise_irq_before` requires `irq_epc_resumable`, and so do the source-profile opcode-boundary checks. |
| dirty_ram_interp.c `precise_pc_dispatchable` | slice hand-back predicate | Claimed "BIOS ROM is dispatchable" for any ROM PC. | **Fixed:** uses the exact gate. Ring site `precise_exit`. |
| dirty_ram_interp.c precise slicer exits 2/3/4 (unsupported, bail, 200k guard) | arbitrary PC | Runaway or undecodable flow. | Unchanged. If the PC is not dispatchable the dispatch now fails loudly, naming `precise_exit`. |
| dirty_ram_interp.c straight-line flow leaving a dirty page | `cpu->pc = pc` | Could hand back mid-block into a clean kernel page (relocated ROM body). | **Fixed:** keeps interpreting instruction by instruction until the PC is re-enterable. Ring site `interp_exit`. |
| dirty_ram_interp.c stop_addr / patch-range end exits | registered return / install-slot resume key | Both are registered keys. | No change. |
| dirty_ram_interp.c 1M-instruction guard yield | arbitrary PC | Runaway. | Unchanged; a non-dispatchable PC fails loudly at dispatch. |
| traps.c / dirty_ram_interp.c `g_async_rfe_resume_pc` | — | Never assigned, so the recovery can never fire. | Dead code, publishes nothing. Follow-up cleanup, not a publisher. |
| bios_hle.c DeliverEvent callback | `psx_dispatch_call(func, deliver_event_ret)` | Profile-exported return key. | No change. |
| debug_server.c `step` | — | Command removed; returns an error. | No publisher. |

### Recompiler (generated code publishes)

| Site | Publishes | Finding | Disposition |
|---|---|---|---|
| full_function_emitter.cpp `jr` / `jr k0; rfe` / jump-table default | `check_at(gpr[rs]); cpu->pc = gpr[rs]` | Read the register twice around the IRQ check. The R3000A latches the target before any exception. | **Fixed:** `emit_publish_expr` latches `psx_pub_pc` once. |
| full_function_emitter.cpp function fall-through | `cpu->pc = last+4` | The only publish that registered no dispatch key. | **Fixed:** `register_cross_function_target(last+4)`. |
| full_function_emitter.cpp `register_cross_function_target` | cross-function branch targets | Silently dropped a target that lies in no discovered function. | **Fixed:** `[cross-target] UNREGISTERED` on stderr at emit. None appear for SCPH1001, SCPH5552, SCPH5500 or OpenBIOS. |
| full_function_emitter.cpp syscall | EPC = syscall; the handler returns +4 | syscall+4 has been a registered leader since 3bd62b309. | No change. |
| full_function_emitter.cpp HLE hook | `cpu->pc = $ra` | Faithful `jr ra`. | No change. Not in the ring; a miss here reports "no runtime publisher". |
| code_generator.cpp (game) `jr` / `jalr` | `check_at(_jt_X); cpu->pc = _jt_X` | Already latched into `_jt_<addr>` before the delay slot. | No change (the audit draft's G1 row was a false alarm). |
| strict_translator.cpp stores / MTC0 | — | No exit and no IRQ check inside a block. | No publisher. |

## Evidence

- Test ROM: `tools/bios_resume_testrom`. It runs ROM A0 routines (memset, memcpy
  and its BFC02B7C delay slot, memcmp, rand, strlen) and Enter/ExitCriticalSection
  syscalls under a root-counter-2 interrupt with an event callback. psx-bresume
  and psx-beetle boot the same synthetic disc. The harness proves both loaded the
  same image by comparing the live shell copy (RAM 0x30000..0x31000) against that
  ROM's 0x18000.. window: psx-beetle loads firmware by filename and runs on when
  the file is absent or is another image (PSX-ORACLE-001). The kernel band at RAM
  0x500 — the bundle README's recipe, and this table's first version — does NOT
  discriminate: ROM 0x10000..0x18000 is byte-identical across SCPH1001, SCPH5552
  and SCPH5500, so all three score 99.95%. The shell band separates them
  100.00% / 21.00% / 17.41%.

  | BIOS | `PSX_PRECISE_SLICE` | shell band | compared words | native callbacks | runtime publishes | refused | unknown dispatches |
  |---|---|---|---|---|---|---|---|
  | SCPH1001 | 0 | 100.0% | match | 81,245 | 87,794 | 0 | 0 |
  | SCPH1001 | 1 | 100.0% | match | 91,505 | 4,893,131 | 0 | 0 |

  SCPH5552 and SCPH5500 cannot be run as a PAIR on this disc: the local license
  data is SCEA, and psx-beetle picks firmware by the disc's region, so the EU/JP
  images cannot be the oracle side here. psx-bresume alone passes on SCPH5552
  (both slice modes), with 0 refused publishes and 0 unknown dispatches.

- BIOS boot pairs, no test ROM: SCPH1001, SCPH5552 and SCPH5500 each reach the
  Sony logo and the shell on both backends, with each process independently
  verified as running the intended image (shell band 100.0% per port, per stem),
  0 unknown dispatches and 0 refused publishes
  (`.../t13-bios-seeds-20260917/boot_image_verified/`).

- Not an interrupt-load parity claim: with the genuine BIOS, psx-beetle delivers
  NO root-counter-2 interrupts (callback count 0) while psx-bresume delivers
  ~90k, and a psx-beetle I_STAT write trace shows only VBlank acks. Same BIOS,
  same EXE, so this is a timer2 target-IRQ divergence, reported separately; it is
  not what this test is for, so only the native count gates the run.

- The audit and test ROM found real publishers (rows marked Fixed), so the
  scope's fallback game boots were not needed.
- Reproducing the August addresses exactly is not possible on this tree:
  instruction-granular ROM execution exists only in the precise/source profiles
  added 2026-09-09. The rows above close every path that could produce those
  shapes.
- No performance regression from these changes: SCPH1001 LLE boot measured on an
  otherwise idle machine is 16.80 fps through the intro and 60.00 fps at the
  shell, against 16.15 / 59.93 for a runtime built at the base commit. (Both
  collapse to a few fps when other builds and emulators run in parallel.)

## 2026-09-18 — exact EPC: the rule above moved the architectural EPC

The rule as written above ("an interrupt that becomes deliverable at a ROM PC
that is not re-enterable stays pending until the next re-enterable boundary")
is faithful about the resume PC and UNFAITHFUL about COP0.EPC. Deferring the
delivery does not merely delay it: it runs the instructions in between, so the
EPC that reaches the guest names a different instruction than the one hardware
would have interrupted.

Pegasus found it on Abe's Oddysee during the T59 batch, on SCPH5501. Base
stored EPC = BFC041D0 where hardware, BizHawk and the pre-merge build all
report BFC041F8; every register matched and frames 98 and 100 agreed, so it was
not a BIOS-image artefact. The ROM explains the pair exactly:

    BFC041EC  8C8C0000  lw   $t4, 0($a0)
    BFC041F0  00000000  nop
    BFC041F4  01856824  and  $t5, $t4, $a1
    BFC041F8  15A0FFF5  bne  $t5, $zero, 0xBFC041D0
    BFC041FC  00601021  addu $v0, $v1, $zero      <- delay slot

BFC041F8 is a branch whose target IS BFC041D0. The interrupt became deliverable
at the branch; `irq_epc_resumable` refused it there, and `exec_delay_slot`
refused it again at BFC041FC (it tests `pc-4`, the same branch); execution
therefore completed the branch and its slot and arrived at BFC041D0, which IS a
block leader — so the delivery happened there. The "dispatchable block leader"
in the report was the interrupted branch's own destination, reached by the
deferral.

**The ruling (Alex, 2026-09-18): EPC stays faithful.** The exact interrupted
instruction is the architectural answer, and it is not negotiable to work
around a resume-side limitation. The alternative — profile-gating the guard so
it only applies where the hazard has been measured — was rejected: it would
qualify a runtime that differs from the shipped one.

### What changed

`full_function_emitter.cpp` only. No runtime source change was needed, because
every gate that was downgrading the EPC reads the same dispatch table. Every
ROM instruction that can be an EPC is now emitted as a continuation, so
`psx_is_dispatchable()` accepts it.

Two exclusions, both correctness requirements and not size optimisations:

- **Delay slots.** The architectural EPC for an instruction in a delay slot is
  the TERMINATOR with `Cause.BD` set (`psx_check_interrupts_delay_slot`), never
  the slot. The slot's emitted body is also guarded on `psx_delay_<term>`,
  which a fresh function entry initialises to 0.
- **The successor of a modeled load-delay pair.** Its emitted body reads, and
  `emit_ldd_flush` writes back, `psx_ldd_<load>` — also zeroed at a fresh entry,
  so entering there would clobber the loaded GPR with zero. The runtime already
  refuses such a PC (`precise_pc_dispatchable` returns 0 while
  `s_ld_pend_armed`), so the two sides agree.

`block_leaders` is deliberately NOT extended. It drives the cycle model
(`psx_slice_block` extents, per-block charges), the I-cache line-leader test,
and the load-delay "dependent pair split by a label" bail-out — adding leaders
would change generated timing and push every ROM function with a dependent load
pair onto the interpreter. A resume point adds a label and a dispatch key and
nothing else.

Continuations are routed by a `cont_pc` column on `DispatchEntry` rather than by
a per-continuation wrapper function. The trampoline already zeroes `cpu->pc`
before every dispatch, so assigning that column immediately before the call is
the same two cases the wrapper had.

### Sizing is PER PROFILE

Dispatch-table membership follows each profile's seeds, so one image's counts
say nothing about another's — SCPH1001 compiles 63,186 instructions and
SCPH5552 19,749, from the same 512 KB of ROM. Do not size this from one image.
The emitter prints its own census for whatever image it was given:

    [T110] <stem> resume points: N functions, N instruction slots =
    N already keyed + N added + N delay slots + N load-delay successors

Measured 2026-09-18 on `t110/epc-continuations`, dispatch entries before/after:

| profile | instructions | entries before | entries after | delay slots | ldd successors |
|---|---|---|---|---|---|
| OpenBIOS | 17,526 | 4,010 | 14,450 | 3,058 | 2 |
| SCPH1001 | 63,186 | 13,338 | 52,316 | 10,835 | 0 |
| SCPH101 | 19,341 | 4,530 | 15,580 | 3,762 | 0 |
| SCPH5500 | 19,749 | 4,644 | 15,878 | 3,868 | 0 |
| SCPH5501 | 54,153 | 11,905 | 44,709 | 9,409 | 0 |
| SCPH5552 | 19,749 | 4,639 | 15,850 | 3,864 | 0 |

SCPH5501 is not a base profile: base ships OpenBIOS, SCPH1001, SCPH101,
SCPH5500 and SCPH5552, and the TAS lane generates SCPH5501 at setup from a
supplied image (`bios/SCPH5501.toml` is committed on the lane, and it reuses
`recompiler/seeds/phase2_ghidra_seeds.json`). It was built here from that lane
file. Base ships six after the lane intake merges. Nothing in this change
hardcodes a profile count.

After this change the only in-function ROM addresses that remain un-resumable
are delay slots — which must stay out — and two load-delay successors in
OpenBIOS.

### Evidence, and what is NOT evidenced

Static, all six profiles, on this revision:

- Generated code is purely additive over the pre-fix emitter. Multiset line
  compare per profile, over 307,742 to 1,071,849 generated lines: exactly one
  line is present before and absent after, the wrapper-count banner whose count
  changed. Added lines classify only as resume labels, entry-switch cases,
  wrapper triples, and an entry-switch skeleton for the 163-584 functions per
  profile that previously had no continuations at all. (This compare is against
  the resume-point commit alone; the `cont_pc` collapse that follows it removes
  the wrapper triples, and is checked separately below.)
- For the reported address: `0x1FC041F8` is a dispatch row; that row enters
  `func_1FC04138` with the label key; that function's entry switch contains
  `case 0xBFC041F8u: goto label_BFC041F8;` (absent before, present after); and
  `label_BFC041F8: ;` sits immediately before the emitted
  `bne $t5, $zero, 0xBFC041D0`, ahead of its i-cache fetch and cycle step, so
  the resume re-executes the interrupted instruction whole and reads its
  operand from `cpu->gpr[13]`, not from any host local.
- The continuation collapse is equivalent row for row: 158,786 dispatch rows
  compared across the six profiles (153,354 continuations, 5,432 plain), 0
  mismatches.

Runtime, `tools/bios_resume_testrom` on SCPH5501 with `PSX_PRECISE_SLICE=1`,
Eagle, gcc 16.1.0: the ROM completed, 59,713 interrupt callbacks, 8,273,067
runtime publishes, 0 unknown dispatches, 0 refused publishes.

**That run does not demonstrate the fix.** Classifying its EPCs against the
before/after tables — 157 ring snapshots, 160,768 publish rows examined, 1.94%
of the stream, 51 distinct EPC targets — gives 21 targets already keyed before
the change, 30 in game RAM (dispatchable without being BIOS keys) and **zero**
that only this change made dispatchable. The synthetic ROM's interrupts do not
land mid-block in ROM, so it cannot reproduce the Abe's Oddysee shape. It is a
no-regression result and nothing more; the title is the only test of the
behaviour itself.

A note on how that number was obtained, because the first attempt got it wrong
in the house style: reading the publish ring ONCE at the end samples its last
1,024 rows out of millions, finds nothing relevant, and reports a clean pass.
Sample throughout the run and print the coverage alongside the verdict.

### Cost

Measured on Eagle, gcc 16.1.0, `-O2 -DPSX_ENABLE_BLOCK_CYCLES
-DPSX_NO_DEBUG_TOOLS`, compiling `generated/SCPH5501_full.c` alone. Interleave
the arms and take more than one round: a fixed-order A/B lets the first run eat
the cold-page cost, and a parallel build on the same machine inflated one of
these by 20% before it was re-run on an idle host.

Object-file growth overstates what ships — most of it is symbol, string and
relocation tables that do not survive linking. The section deltas are the
honest number: `.text` +1.18 MB, `.rdata` +0.69 MB, `.pdata` +0.39 MB, `.xdata`
+0.13 MB, so roughly +2.4 MB linked per profile. `.pdata` grew 393,624 bytes
over 32,804 new continuations, 12.0 bytes each — that is the per-wrapper unwind
record, one for one, which is what the `cont_pc` collapse removes.

`full_function_emitter.cpp` is already listed in `codegen_hash_sources.cmake`,
so this change moves the codegen hash and invalidates overlay caches by design.
Kits must regenerate: there is no stale-cache hazard, and no cache reuse either.
