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

## Finding 4 — finite textured backdrops need semantic OT ownership

Status: confirmed in SF2 Mission 1; independent validation pending.
Likely owner: native-wide GPU primitive classification.

A finite authored background mesh may leave newly revealed widescreen margins
even when world projection is correct. Expanding far GTE geometry, every
textured primitive, a palette, or a packet address either damages foreground
geometry or encodes title payload. For an opted-in title whose draw ordering is
stable, the first consumed ordering-table rank that submits textured polygons
is a bounded semantic owner. Stretch only textured polygons from that rank in
the wide mirror; preserve the canonical 4:3 framebuffer.

Bounded check:

1. Census GP0 polygon types by consumed OT rank for the affected frame.
2. Falsify global and exact-call GTE expansion against world/actor geometry.
3. Use raw texture/source identities only as temporary correlation probes.
4. Toggle the semantic owner live under software and hardware renderers.
5. Require the reveal gap to change while HUD, actors and foreground remain
   unchanged; add a frame-reset unit regression.

SF2 result: the outdoor black reveal wedge returns with the owner disabled and
disappears when enabled under both backends. Software high-resolution polygon
seams are unchanged by the toggle and remain a separate renderer issue.
Production configuration contains no SF2 address, packet range, palette,
texture identity, movie dimension or hardcoded band.

First independent consumer: **Tenchu**, which can classify its first outdoor
background frame by OT provenance without importing SF2 identities.

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
- Remaining independent status: GPU and CD findings are still open for a second
  title before stable cross-portfolio promotion.

## Reproduction references

- Generic commits: `09be64b`, `485b79b`, `89804a7`.
- Full local report: `OVERNIGHT_REPORT_2026-08-03.md`.
- Complete framework suite: 43/43.
- No BIOS, disc, executable, generated game code, captures, RAM, movies, audio,
  screenshots, cards, or other private artifacts are included.
