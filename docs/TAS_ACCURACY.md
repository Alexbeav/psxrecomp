# TAS accuracy profile and Tekken 3 validation

This submission ports the TAS comparison profile onto upstream master
`85cd26f0`. See [integration status](TAS_UPSTREAM_INTEGRATION.md) for the
current checks, source mapping and exclusions. The results below were measured
on the earlier campaign builds, whose base was `f23c5ba1`; they do not qualify
this newly integrated runtime. Its overlay callback ABI is 24, preserving the
upstream v23 callbacks before appending TAS instruction-boundary callbacks.

The unchanged Spikestuff movie contains7,974 inputs. The integrated native
build wins Arcade at8.80 seconds. Every original-input return clock and all512
4KiB RAM-page hashes match the independent Octoshock2.2.2 reference;28 full
2MiB snapshots are also byte-identical. This is stronger evidence for this
route than a clean process exit or simply consuming the input file. It is not
a proof of complete hardware or cross-title compatibility.

## Corrections integrated

Related corrections are grouped here by their shared mechanism. Snapshot
numbers in the original research included diagnostics and experiments; they
are not bug counts.

| Area | Corrected mechanisms |
|---|---|
| CPU/code generation | Uncached instruction fetch costs; copied BIOS/RAM alias admission and syscall EPC; exception transfer and post-SYS continuation; deferred multiply deadlines; isolated cache-tag stores; ROM/RAM precision guards and conservative block bounds; pending-load ownership across compiled/interpreted boundaries; signed arithmetic exceptions; GTE load cancellation, delayed register values, and COP2 interrupt ordering. |
| Interrupts | Execute the actual vector instructions; interrupted fetch and branch-delay context; EPC/Cause/BD/BT/TAR ownership; IRQ reentry/cooldown and repeated delivery after RFE; Cause CE bits; register read image and sampling; recognize IRQs after DMA halt; VBlank latching during a guest IRQ handler. |
| DMA | Live request/linked-list GPU transfer; upload/readback readiness and word ownership; cancellation at packet boundaries; initial credit and register-write order; OTC/CD CPU stalls and instruction fetch overlap; completion deadline invalidation; fractional-service/frontend ordering; SPU DMA initial/word/block timing. |
| GPU | One FIFO/service owner for command admission, dispatch and visible effects; phase-correct triangle rendering and queued environment state; A0/C0 transfers; polygons, textures/CLUT/cache, shading, masks and blending; fill/copy/rectangles; clipping and VRAM wrapping; field-aware row work; raster/status timing and automatic frame-return ownership. |
| CD-ROM | Implicit and explicit seek transitions; asynchronous sector IRQ presentation; firmware/cold tray state; TOC/seek/read pipeline and command clocks; reset; Pause ACK and head rewind/resume; trigger/read-head state. |
| Timers and SIO | Source raster field clock; timer1 HBlank and read sample; timer2 divider/IRQ/deadline; digital-pad ACK pulse; opt-in handling of an unrelated legacy card repair. |
| SPU | Delayed control/status visibility with independent sample service; key-on/envelope phase; ADPCM decode queue, END flags, pitch/filter state and guest register readback. |
| Overlay integration | ABI24 forwards live instruction-boundary callbacks and pending cycle ownership through generated DLLs, preserving replay exclusion and rejecting stale ABI caches. |

Most detailed timing changes are enabled by explicit `octoshock-2.2.2` profile
options; they model the exact emulator on which this TAS was authored.
General CPU/controller defects also have source-level corrections. The profile
does not contain a title-name switch, a precomputed game-state stream, or
delays chosen to force an opponent or victory.

The reference is the immutable
[BizHawk source commit](https://github.com/TASEmulators/BizHawk/tree/519e14aa1ad7a9d6df2edc7808c5ed687dfee046),
observed using ordinary authored guest programs and original movie playback.
The native CPU, devices and renderer execute independently. The isolated
external raw PRNG utility has a separate GPL license and file interface;
its arithmetic is not imported into the framework runtime.

## Reproduction and validation boundaries

The recipe generates the BIOS and title from owned assets using the repaired
emitters. All 57 resulting C files must match the retained historical
fingerprints. Until regenerated code and a fresh retail replay are qualified,
a fingerprint mismatch correctly prevents this integration from being advertised
as the earlier verified build. It builds dependencies from pinned archives and uses
software rendering in both visible and headless modes. Recompiler and authored
O0/O2 tests run without retail assets. The final replay verifier rejects an
input mismatch, a missing or reordered checkpoint, a different RAM-page hash,
a return-clock difference, or incomplete termination.

Historical C fixtures that require retained original-core captures are still
identified as fixtures by the registration checker. Their old private driver
names describe provenance, not a claim that those drivers ship in this branch.
Portable CTest registrations and the complete retail replay are the repeatable
checks available from the clone.

The ending is explicitly bounded at8,400 frames:7,974 original inputs plus426
neutral inputs, through the visible victory. Later CDDA Play seek remains
unqualified and stopped the research tail after return10,704. PAL, save-state
restoration, other input devices, other titles, complete audio/pixel equality,
and unrestricted post-victory playback are outside this qualification.
