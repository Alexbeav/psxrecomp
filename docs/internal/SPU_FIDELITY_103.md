# SPU fidelity burndown — Issue #103 and PR #102

Branch: `feat/spu-fidelity-103`  ·  Worktree: `_wt-spu-103`  ·  Base: `origin/master` `7f7fbc8e`

Closes the four gaps named in Issue #103 (reverb, noise, volume sweeps, SPU IRQ)
and lands the separable, verified parts of PR #102. Written so the disposition of
each piece is auditable rather than asserted.

---

## 1. Scope overlap between #102 and #103

They intersect on exactly one item.

| Item | Issue #103 | PR #102 | Disposition |
| --- | --- | --- | --- |
| Reverb | ✔ asked | — | Implemented here, clean-room (§3) |
| Noise LFSR | ✔ asked | — | Implemented here, clean-room (§3) |
| Volume sweeps | ✔ asked | — | Implemented here, clean-room (§3) |
| SPU IRQ | ✔ asked | ✔ offered | **One implementation**, ours, in `spu.c` |
| CAUSE.IP2 combinational | — | ✔ offered | Taken, verified, extended (§2) |
| Mid-dispatch audio pump | — | ✔ offered | Taken, with a regression fixed (§2) |
| SPU RAM DMA readback | — | ✔ offered | Taken as-is in substance (§2) |

So this is one coherent change, not two competing ones. PR #102 covers 1 of
#103's 4 items; the other three were never in it.

## 2. PR #102 — per-piece verdict

PR #102's four changes were reviewed as four separable items, because they carry
very different risk. Attribution to Alexandros Mandravillis is recorded in the
commit trailer and in the source comments; the code here is a reimplementation
on the same findings, because two of the PR's three commits touch `spu.c` and
would have collided with the clean-room work.

### 2a. CAUSE.IP2 combinational — CONFIRMED, taken, and extended

The PR's claim is that `CAUSE.IP2` is a live mirror of the interrupt line rather
than a latched bit. **The claim is correct**, and it was checked against the
in-tree Beetle oracle rather than accepted on the PR's reasoning:

- `beetle-psx/mednafen/psx/irq.cpp` defines
  `#define Recalc() PSX_CPU->AssertIRQ(0, (bool)(Status & Mask))`
  and calls it from `IRQ_Assert` (raise), from **both** halves of `IRQ_Write`
  (the Status ack and the Mask write), and from `IRQ_Power`.
- `beetle-psx/mednafen/psx/cpu.cpp:289 AssertIRQ()` clears `CAUSE` bit `10+n`
  unconditionally, then re-sets it only if the level is asserted.

That is the definition of a combinational mirror, and it matches R3000A: the
`Cause.IP` field is not storage, it reflects the interrupt pins. Our runtime
previously only ever OR'd bit 10 in at delivery and never cleared it.

Extensions beyond the PR:

1. **A second latching writer the PR missed.** `runtime/src/psx_interpreter.c`
   had its own `cpu->cop0[COP0_CAUSE] |= (1u << 10);` at interrupt delivery —
   the identical defect in the standalone interpreter path. Fixed too. An audit
   of every writer of `cop0[13]` confirms no other site touches bit 10: the
   interpreter's `MTC0` paths correctly mask guest writes to `0x0300`
   (software-interrupt bits only), and the exception paths mask `~0x7C` / 
   `~0x8000007C`, preserving the IP field.
2. **Single ownership.** The compiled delivery path no longer sets bit 10
   itself; `psx_irq_refresh_cause_ip2()` is the only writer. Two writers of one
   combinational bit is how the bug survived in the first place.
3. **Power-on recompute**, mirroring Beetle's `IRQ_Power() -> Recalc()`.
4. **A regression test**, `runtime/tests/test_cause_ip2_combinational.c`,
   covering rise, fall on ack, fall on mask, partial ack with another source
   still pending, partial-width register writes, preservation of ExcCode/BD/IP0/
   IP1, and out-of-range I_STAT bits. Registered in CTest.

Risk note: this is a real behavioural change to core IRQ delivery for every
title, and it is the highest-risk item in this branch. It is nonetheless the
faithful behaviour, so per Rule -1 it lands and titles are revalidated rather
than the fix being narrowed.

### 2b. Mid-dispatch audio pump — taken, with a regression the PR would have shipped

The reasoning is sound: the SPU is autonomous on hardware, our pump is driven
from the main loop between presented frames, so a guest busy-wait that never
presents a frame freezes SPU time — self-deadlocking for any game waiting on an
SPU-generated condition. The VBlank edge is derived from the guest cycle
counter, so it keeps firing through such a wait and is a defensible place to
pump. Confirmed single-threaded: `spu_render` runs on the main loop thread
(`sdl_audio_pump` is the producer; the SDL callback only drains an SPSC ring),
so this introduces no data race on `i_stat`/`cop0`.

**Defect found and fixed.** PR #102 wires the hook to `sdl_audio_pump()`
unconditionally. But `sdl_audio_update()` is the sole authority on whether a
pump should emit, discard, or not run:

- hard mute (turbo loads) → *does not pump at all*; the queue drains and voice
  positions freeze in place, so music resumes where it left off instead of
  replaying time-compressed garble. This is user-validated behaviour.
- turbo sink → pumps with `discard_output = true`.

Pumping unconditionally from the VBlank edge bypasses both, which would push
real audio during every turbo load. The hook now goes through
`sdl_audio_pump_midframe()`, which mirrors the gate state
(`AUDIO_GATE_NORMAL` / `MUTED` / `SINK`) that `sdl_audio_update()` last set.

Known tension, recorded rather than guessed at: in the `MUTED` state SPU time
still freezes, so a game that busy-waits on an SPU condition *during* a turbo
load would stall. No title in our suite is known to do this. Advancing SPU time
under mute would change validated mute audio, so it is not being done
speculatively.

Placement: the pump call sits at the **end** of `fire_vblank_edge()` so the
VBlank's own raise and ring records complete first — the pump can itself raise
an SPU IRQ, which should land after the edge rather than interleaved into it.

### 2c. SPU RAM DMA readback — taken

DMA4 in the SPU→RAM direction previously wrote literal zeros. SPU RAM is
readable memory; this was not a transfer at all. Titles that carry a checksummed
block through SPU RAM across an `Exec` boundary (SPU RAM being one of the few
regions a main-RAM reload does not touch) read zeros, failed their own integrity
check, and fell back to a cold-boot path. Lowest-risk item in the PR:
previously-dead path, no existing behaviour depends on the zeros.

### 2d. Validation claims — NOT reproduced, and cannot be

PR #102 validates against Medal of Honor Underground (SLUS-01270) and Gran
Turismo (SCUS-94194). Neither disc is in our suite. Those specific claims are
**unverified by us and should not be repeated as if they were**. What we
verified independently is the hardware behaviour behind each change (§2a–2c) and
non-regression across our own titles.

## 3. Issue #103 — why this had to be clean-room

⚠ **This is the load-bearing constraint on the whole item, and it is why #103
sat open.**

`psxrecomp` is distributed under **PolyForm Noncommercial 1.0.0**. Beetle PSX /
mednafen is **GPL-2.0-or-later**. They are incompatible: GPL source cannot be
folded into a PolyForm distribution. Beetle's tree contains a complete, correct,
well-tested reverb — so the obvious implementation route is closed, and two
contributors independently walked into it:

- **PR #16** (Martin Penkava) implemented all of this by porting Beetle. Parked
  on license grounds after a line-by-line audit found it tracked
  `beetle-psx/mednafen/psx/spu.cpp` lines 590–730 in register layout, resampling
  coefficients, buffer layout, arithmetic and processing order. See
  `docs/internal/upstream/martin-pr16-spu-reverb.md` on branch
  `audit/pr16-spu-reverb-gpl-lineage-mpenkava`.
- **PR #13** (parked at `5fc8e15d`) shipped a reverb too, rejected for being
  *wrong* rather than for licensing: it gated reverb on bit 15 of `0x1F801DC0`,
  which is `dAPF1` — an all-pass *offset* register whose top bit is set for any
  offset ≥ `0x8000`, so reverb switched on and off according to an address
  value. The real gate is SPUCNT (`0x1F801DAA`) bit 7. It also read
  `dAPF2` / `vIIR` / `vCOMB1` as "feedback" / "wet level" / "early-reflection
  level"; they are none of those.

The PR #16 audit names the only acceptable path, and this branch takes it: **a
clean-room implementation from hardware documentation, with independently
derived tests.** The psx-spx / nocash register map and documented algorithm were
the source. Beetle's source was deliberately not consulted for the
implementation.

Beetle remains fully usable as a **runtime oracle** — running the binary and
comparing its audio output is not a derivative work, and that comparison is how
the remaining uncertainties in §5 get settled. `audio_wav` and `audio_stats`
exist on both `psx-runtime` and `psx-beetle`, so the comparison is a symmetric
always-on ring query, not an arm-then-capture.

### Pre-existing lineage exposure — reported, not changed

Separate from this work, and worth a decision:

- `runtime/src/spu.c` `calc_vc_delta()` is commented *"Ported verbatim from
  Beetle's CalcVCDelta"*. This is the ADSR rate decoder, and it is in every
  released binary. Same category as PR #16, already shipped.
- `runtime/include/spu_gauss.h` cites *"No$PSX docs / DuckStation
  core/spu.cpp"*.

Deliberately left alone here: rewriting the ADSR rate decoder clean-room changes
every voice envelope in every title and needs its own revalidation campaign, not
a rider on this branch.

## 4. `feat/pr13-salvage-remainder` is fully superseded — no surgery needed

Prior notes flagged this branch as "ready to land" while Issue #103 says its
reverb must stay parked, and treated reconciling that as a blocker. **It is not
a blocker: the branch retains nothing but the rejected reverb commit.**

`git cherry -v origin/master feat/pr13-salvage-remainder`:

```
- e9a8a24e  interp: deliver alignment exceptions, protect Cause read-only bits, add CFC0
- 3ce30155  dma: implement channel 5 (PIO / expansion port) transfers
+ 5fc8e15d  spu: reverb, noise LFSR, sweep volumes, pitch modulation, SPU IRQ
- 9f3cd0a3  autocompile: name the resolved interpreter at configure, warn on Cygwin builds
```

Three of four are already upstream (`-`). Verified four independent ways rather
than trusting patch-id alone:

- cherry-picking all three onto current `origin/master` produced **empty**
  commits;
- `execute_ch5_pio()` is present in `runtime/src/dma.c` on master;
- `CFC0` handling and the `& ~0x0300u | (val & 0x0300u)` Cause write-protection
  are present in `runtime/src/dirty_ram_interp.c` on master;
- `git diff origin/master 9f3cd0a3 -- runtime/src/autocompile.c` is **empty**.

Consequences: there is no salvage left to preserve, no git surgery to perform,
and the "two competing SPU IRQ implementations" question resolves by default —
`5fc8e15d` is both superseded and Beetle-derived. The branch should be
abandoned and PR #13 closed. `origin/park/pr13-spu-reverb-rejected` still
retains the commit, so nothing is lost.

Incidentally, `e9a8a24e`'s Cause write-protection (guest `MTC0` may only write
bits 8–9) is exactly what makes §2a safe: the guest cannot forge IP2.

## 5. Known deviations and the oracle-verification queue

Recorded explicitly rather than presented as exact (Rule 14).

- **22.05 → 44.1 kHz reverb reconstruction filter.** The reverb engine runs at
  22050 Hz, but the hardware's reconstruction filter is not specified in the
  documentation used. Beetle's 39-tap FIR was deliberately not copied. The
  implementation uses a clearly-marked, single-function reconstruction so the
  filter can be swapped once measured against the oracle. **This is the one
  place the implementation is known to differ from hardware.**
- Further per-feature judgement calls are listed in §6 as reported by the
  implementation pass; each is a candidate for oracle comparison.

## 6. Implementation judgement calls

_Populated from the implementation pass; each entry is an oracle-comparison
candidate rather than a settled fact._

## 7. Validation status

- [x] `test_cause_ip2_combinational` passes (`-Wall -Wextra -Werror`).
- [x] Changed C translation units compile clean.
- [ ] `test_spu_fidelity` passes.
- [ ] `psx-runtime` links; BIOS boots; boot chime audible and non-distorted.
- [ ] Title run: no regression, reverb present.
- [ ] Oracle audio comparison against `psx-beetle` at a fixed scene.
- [ ] User ear-validation (final gate).

## 8. Incidental fix — `tools/embed_spirv.py` build race

Not SPU-related, found while building. Two runtime targets (`psx-runtime`,
`psx-oracle`) embed the same shader set, so ninja runs `embed_spirv.py`
concurrently. It wrote its intermediate SPIR-V to `<source>.spv` **in the source
tree**, so the two invocations raced on one path and whichever finished first
`os.remove()`d the file the other was about to read — a hard build failure
(`FileNotFoundError: .../blit.frag.spv`), and source-tree pollution besides.
Now compiled to a private `mkdtemp` directory, with the output header written
via atomic replace and a loud error if `glslc` reports success but produces no
file. Fixed per Rule 15 rather than worked around.
