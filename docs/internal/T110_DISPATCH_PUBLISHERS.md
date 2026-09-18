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
| interrupts.c `psx_check_interrupts_delay_slot` | `EPC = branch` with BD/BT | Faithful, but a ROM branch is not re-enterable. | **Fixed:** deferred when the branch PC is not resumable (`irq_epc_resumable`). Ring site `epc_delay_slot`. **This row states the INTENT, not the behaviour — see T163 below.** |
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

## T163 — what this audit did not cover (2026-09-18)

Two defects traced to this work after it shipped. Neither is in the table
above, and the reason they are not is the same in both cases: **the audit's
subject was sites that PUBLISH a resume PC, but the change's blast radius was
every CALLER of `psx_is_dispatchable`.** Those are different sets, and both
defects live in the difference.

### 1. The delay-slot row states intent, not behaviour

The `psx_check_interrupts_delay_slot` row says the take is "deferred when the
branch PC is not resumable". The code does something stricter. `exec_delay_slot`
(dirty_ram_interp.c) gates on

```
precise_irq_before(cpu,pc) && irq_epc_resumable(pc-4u)
```

and `precise_irq_before` itself contains `irq_epc_resumable(pc)`. At a delay
slot `pc` IS the slot, so the gate also requires the SLOT to be a dispatch key
— which the emitter deliberately guarantees it never is. The two terms
contradict each other and the first always loses: the take is not deferred, it
is **unreachable**, for every BIOS-ROM delay slot. Measured on SCPH5501: the
gate was satisfiable at at most 15 of 9,331 ROM terminators.

The interrupt is not lost — `exec_one_fetched_context` takes it with the same
EPC and Cause one boundary later, using the correct test
(`irq_epc_resumable(in_slot ? pc-4u : pc)`).

Left as-is deliberately. A candidate fix restoring the intended behaviour was
built and routed (Abe's Oddysee, route 04) and was **byte-identical to the
unfixed run across all 49,854 returns** — inert on that route. It is a real
contradiction with no measured consequence, so it is not worth an unvalidated
change to the interrupt path. Do not "fix" it without a test that exercises it.

### 2. A predicate whose meaning changed under an untouched caller

`interrupts.c` (delivery path) charges the preempted opcode's I-cache fetch and
one base step. Its own comment states the intent: *"Preserve that fetch's tags
and load-absorb effects, but never execute its register/store effects."* It is a
timing charge. The ROM branch of its guard read `&& psx_is_dispatchable(fetch_pc)`.

That term was a **tautology when written** (5cda8cf9c, which introduced the
guard and the comment together): `psx_is_dispatchable` then returned 0 only for
`pc == 0` and `PSX_EXC_SENTINEL_PC`, and neither can reach that branch —
`fetch_pc != 0u` is tested two lines above, and the sentinel is `0x80000048`,
whose phys `0x48` takes the RAM branch.

Making `psx_is_dispatchable` exact turned that tautology into a behaviour
change **without touching the line**, so it does not appear in this work's diff
at all. For a delay-slot take `fetch_pc` is the SLOT, which is deliberately
absent from the dispatch table, so every ROM delay-slot take stopped charging
its preempted fetch: 4 cycles for the KSEG1 miss plus 1 for the base step. The
machine then runs one instruction ahead of itself from that exception onward.

Abe's Oddysee, against a 970ab88d control on the same host: returns 700–703 one
instruction early at an identical clock, converting to five cycles fast from
return 704. Removing the term returns all four predicted rows to the source
values, and the 710-return prefix passes where the head fails at 702. Fixed by
dropping the term; the remaining tests (non-zero, aligned, in RAM or the BIOS
window) are what the guard actually requires, and `psx_icache_fetch` reads no
guest memory.

**The trigger is not title-specific.** Returns whose sampled EPC is in the BIOS
window with `Cause.BD` set, across the eight requal-03 routes — a lower bound,
since a return shows only the last exception: abesoddysee 37, biohazard 39,
crash 30, megamanx4 72, megamanx5 35, pepsiman 39, redc 39, tekken3 21. Every
title meets it. pepsiman and tekken3 passed requal-03 *before* this change and
had not been re-run since, so the fix restores the behaviour they passed under.

### The rule this leaves behind

**When a change alters what a predicate MEANS, the review set is its callers,
not its subject matter.** A caller written against the old meaning changes
behaviour silently and shows up in no diff of the commit responsible.

Screen for the second defect's shape mechanically rather than by reading names:
*does the guarded block charge cycles?* A timing path and a control-flow gate
are indistinguishable by variable name and obvious by that test. Applied to all
15 gating call sites of `psx_is_dispatchable`, exactly one guards a block that
calls `psx_icache_fetch`, and it is the defect above. (A screen, not a proof: it
reads 22 lines into each block, so a charge further in or behind a call is
missed.)

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
