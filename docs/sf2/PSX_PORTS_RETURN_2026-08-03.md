# PSX-Ports return package — SF2 recomp 2026-08-03

This document is payload-free and intended for the private PSX-Ports knowledge
corpus. It contains generic contracts and falsifiable validation steps only.

## Finding 1 — split GPU VRAM ownership across depth changes

Status: confirmed in SF2; independent validation pending.  
Likely owner: hardware-renderer VRAM coherency.

When a hardware backend keeps GPU drawing in an FBO but presents 24-bit packed
RGB from a CPU mirror, a 15-to-24 transition must transfer ownership before the
first CPU movie upload. Otherwise correct partial uploads expose stale CPU data
outside their rectangle. A blanket black-band rule is incorrect because PS1
VRAM persistence is authored behavior.

Bounded check:

1. Capture GP1 depth transitions and the bounded fill/copy/upload sequence.
2. Compare software VRAM with hardware FBO/CPU representations at the first
   depth-24 scanout.
3. On 15-to-24 transition, synchronize prior FBO-owned work into CPU VRAM.
4. While depth 24 owns packed CPU data, mirror fills that affect scanout into
   both representations and do not read back over subsequent CPU uploads.
5. Require deterministic framebuffer/display evidence under both backends.

SF2 result: affected OpenGL evidence changed from 292 mismatches to zero;
software remained correct. Regression: `gl_depth24_coherency`.

First independent consumer: **Tenchu**, because its recomp lane already reaches
a retail intro FMV and can repeat the OpenGL/software transition check without
sharing SF2 movie dimensions or addresses.

## Candidate 4 — first-textured OT rank is not backdrop ownership

Status: contradicted in SF2 Mission 1. Do not promote.
Likely owner: still-open native-wide background/SCRIM classification.

The first consumed ordering-table rank that submits textured polygons is not a
safe semantic backdrop owner. Human parachute-scene evidence showed correct
canonical 4:3 rendering but enlarged, discontinuous world geometry in both
revealed margins. The bounded GP0 dump proved that rank contains connected
projected `0x3C`/`0x34` environment polygons, not an independent flat image.
Aspect-stretching each primitive transforms real curved geometry twice.

Reusable rejection test:

1. Capture the affected rank's bounded GP0 topology, not only its texture or
   ordering provenance.
2. Reject the owner if adjacent polygons form a projected environment mesh or
   if their edges/vertices extend beyond the authored display.
3. Compare the canonical centre and both reveal margins under a live toggle.
4. Remove the production opt-in when the rule changes real world geometry;
   reopening a finite reveal is preferable to a presentation containment.

SF2 result: `nw_phase_backdrop` is disabled in the production profile. Global
far-depth GTE, exact GTE callers, raw palette/source identity and first-textured
rank ownership are all rejected. Semantic background ownership remains open.

## Candidate 5 — partial-height fullscreen effects use authored coordinates

Status: confirmed by hidden software/OpenGL semantics; human and independent
validation pending. Likely owner: native-wide GPU effect classification.

Fades, cinematic mattes, scope/NVG bands and filters can own the complete
horizontal output without covering its complete height. Recognize only an
axis-aligned rectangle/quad spanning both authored horizontal edges; height is
deliberately irrelevant and projected non-axis-aligned world geometry must not
match. Perform the test in packet coordinate space: a title may author the
screen around zero and apply GP0 draw offset later.

Bounded check:

1. Derive the authored left edge from the live framebuffer/draw-area origin
   minus GP0 draw offset; do not assume packet X begins at zero.
2. Test rectangle and flat/Gouraud/textured quad families with the same pure
   predicate and expand only their horizontal edges into reveal margins.
3. Do not subsequently apply HUD corner re-anchoring to a recognized effect.
4. Reject narrow and projected/non-axis-aligned quads in a title-neutral unit.
5. Require zero expansions in 4:3-owned TITLE/FMV/briefing phases, then nonzero
   expansions in the affected native-wide scene under both renderers.

SF2 result: the origin-naive implementation reached state 0 with 65,148 checks
and zero expansions. With authored origin `-192` derived from draw offset
`+192`, clean software and OpenGL routes passed state-0 movement with exactly
7,035 expansions at player ownership. Regression: `ws_fullwidth_effect_test`.

First independent consumer: **Tenchu**, whose fullscreen fades/mattes can test
the same topology/origin rule without importing SF2 coordinates or effects.

## Finding 2 — SeekL/SeekP owns the CD drive state

Status: confirmed in SF2; candidate for cross-title validation.  
Likely owner: CD-ROM device lifecycle.

SeekL/SeekP must cancel an active ReadN/ReadS generation and its pending
data-ready ownership before setting SEEK. Allowing the old stream to continue
can make new SetLoc targets invisible even though retail issued the correct
commands.

Bounded check:

1. Begin a read stream at location A.
2. Issue SetLoc B and SeekL/SeekP without first pausing.
3. Prove no old-location sector or pending INT1 survives the seek.
4. Prove the subsequent read begins at B and READ/SEEK status changes in order.

SF2 result: restored Eidetic and ZINTRO in the authentic no-input startup.
Regression: `cdrom_seek_retarget`.

## Finding 3 — deterministic automation needs guest-frame input ownership

Status: confirmed validation contract.  
Likely owner: test/debug infrastructure.

Host socket arrival time is not a deterministic input boundary. A diagnostic
input command should be armable ahead of an exact guest frame and should report
application from the emulation-thread consumption point. Frame-history or live
PAD observation can be ordered before application or can miss a short pulse.

Bounded check:

1. Arm input before guest frame N.
2. Prove neutral input before N.
3. Record first/last applied guest frame, value, and count where the emulation
   thread hands input to the device model.
4. Compare two clean processes using identical scheduled records.
5. Compare guest state and cumulative fingerprints on intersecting exact guest
   frames; normalize only explicitly classified host/query variants.

SF2 result: identical startup and input hashes, exact matching RAM/PC/MMIO/
scratchpad/cycle fingerprints at five checkpoints, and identical final player
state. Regression: `debug_input_schedule`.

## Corpus lead dispositions

- `PSX-GPU-002` / `FAIL-009`: **confirmed** as the OpenGL symptom and owner.
- `PSX-GPU-001`: **narrowed** to the persistence constraint; it rejects blanket
  band clearing but is not itself the defect.
- `PSX-MDEC-004`: **irrelevant** to this case; software decoded and presented
  the identical retail stream correctly.
- Early-input propagation: **contradicted** as the cause of missing Eidetic and
  ZINTRO by a fully neutral no-input control.
- Native overlay/code generation: **contradicted** for the startup omissions by
  native/interpreter parity and exact CD command history.
- `PSX-HLE-001`: **narrowed** to a useful complete-device-state lesson; the
  confirmed owner here is the LLE CD device's SeekL/SeekP lifecycle.
- Hardcoded movie geometry or black bands: **irrelevant/rejected containment**.
- First-textured OT-rank background ownership: **contradicted** by projected
  environment topology and human margin corruption.
- Full-width effects: **confirmed under software/OpenGL semantics** with the
  authored coordinate origin included; human and independent-title checks remain.
- Remaining independent status: GPU and CD findings are still open for a second
  title before stable cross-portfolio promotion.

## Reproduction references

- Generic commits: `09be64b`, `485b79b`, `89804a7`.
- Full local report: `OVERNIGHT_REPORT_2026-08-03.md`.
- Complete framework suite: 49/49.
- No BIOS, disc, executable, generated game code, captures, RAM, movies, audio,
  screenshots, cards, or other private artifacts are included.
