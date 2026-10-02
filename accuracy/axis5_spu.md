# Axis 5 — SPU (Audio) Accuracy Findings

**Status: a historical audit, rewritten on 2026-10-02 (PS1B-216).** The audit was
made in June 2026 against a `runtime/src/spu.c` of 799 lines. It compared our
SPU with the oracle emulator and with PSX-SPX "Sound Processing Unit (SPU)".
The first text of this file described the oracle's internals and cited its
source. That text is removed: this file now states each finding as hardware
behaviour, with its PSX-SPX section, and says where the finding stands today.
Line numbers of the June file are not kept; they no longer match.

Two statements of the first text need a plain record:

- It said the ADSR envelope of that time was a port of the oracle's code. That
  was true of the June file. The envelope was replaced on 2026-09-26
  (PS1B-192) by `runtime/include/spu_envelope.h`, written from PSX-SPX
  "Envelope Operation depending on Shift/Step/Mode/Direction" and from oracle
  fixtures (set S-spu, E1 to E9). The June functions no longer exist.
- It recommended porting the oracle's reverb, sweep, noise and pitch-modulation
  code. That was not done. Reverb, noise, sweeps and the IRQ compare were
  written from PSX-SPX (issue 103; see `docs/internal/SPU_FIDELITY_103.md`).
  The licences do not allow a port, and the method does not either.

## 1. What the June implementation did

- 24 voices, mixed in `spu_render`.
- ADPCM block decode in whole 28-sample blocks, with the filter table of
  PSX-SPX "SPU ADPCM Samples" and `cdromformat.md` "XA-ADPCM" (0, 60, 115, 98,
  122 and 0, 0, -52, -55, -60).
- An ADSR envelope (replaced since; see above).
- Key On and Key Off as PSX-SPX "Voice 0..23 Key ON/OFF" describes: Key On
  starts Attack at level 0 and clears the ENDX bit; Key Off starts Release from
  the current level.
- The ENDX latch (1F801D9Ch) and the current ADSR volume read-back
  (1F801C0Ch+N*10h).
- CD audio input mixed under SPUCNT bit 0 with the CD volume registers.
- DMA and manual transfers into SPU RAM.
- Pitch as a 12-bit fractional step.

## 2. What the audit found missing or wrong, and where it stands

| # | Finding in June | PSX-SPX section | Today |
|---|---|---|---|
| 2.1 | Samples were produced when the host audio queue asked, not on the guest clock. Envelope and release rates drifted against guest time. | "Unstable and Delayed I/O": the SPU works at 44,100 Hz, one step per 300h CPU cycles | Fixed: the SPU runs on the guest sample clock (`spu_sample_last_cycle` in `spu.h`; the 768-cycle service in `interrupts.c`). |
| 2.2 | No reverb. | "SPU Reverb Formula", "SPU Reverb Registers" | Written from the documented formula. One known gap: the filter at the 22.05 kHz to 44.1 kHz boundary is not documented; ours is marked in the code. |
| 2.3 | No noise generator. | "SPU Noise Generator" | Written from the documented timer and shift register. The divider wiring is the least documented part. |
| 2.4 | No pitch modulation. | "Voice 0..23 Pitch Modulation Enable Flags (PMON)" | Still absent on the default path. |
| 2.5 | No volume sweeps; the fixed-volume scale was off by a factor of two. | "SPU Volume and ADSR Generator" | Fixed: sweeps use the same documented formula as the envelope (`spu_envelope.h`); fixture class E9 covers them. |
| 2.6 | No IRQ on the IRQ address. | "SPU Interrupt" | Written from the document. Fixture S5 records one oracle observation of IRQ 9 after a register write; the compare on a voice fetch has no fixture yet. |
| 2.7 | No capture buffers. | "SPU Memory layout", "Capture Interrupt" | Written; fixture E8c covers the ring while the SPU is disabled. |
| 2.8 | SPUSTAT was a constant. | "1F801DAEh - SPU Status Register (SPUSTAT)" | Composed from the applied mode bits, the IRQ flag and the capture-half bit; fixture S4. |
| 2.9 | A reserved ADPCM shift (13 to 15) was treated as 12. | `cdromformat.md`: reserved shifts act as shift 9 | The default path still uses 12. The source-profile decoder follows a measurement. Rare in real data. |
| 2.10 | An End block without Repeat kept the level and decoded forward. | "Flag Bits": End+Mute jumps to the loop address, sets Release and level 0 | Fixed; fixture S1. |
| 2.11 | A write to the repeat address after Key On was lost at the next Loop Start block. | "Voice 0..23 ADPCM Repeat Address" | The default path takes the written address at once; whether a later Loop Start block replaces it there is release policy. The source profile follows fixture S2. |

"Fixture" means an oracle observation recorded from an authored program; the
sets are named in `runtime/include/spu_envelope.h`, `runtime/src/spu.c` and the
SPU tests.

## 3. Validation method

- Compare with the oracle as a running process: its audio output and its debug
  payloads (`spu_voices`, `spu_global`, the SPU event ring) against ours, over
  the debug protocol, for the same scene and the same guest frame window.
- Pin single behaviours with oracle fixtures from authored programs, and replay
  the fixture rows in unit tests (`test_spu_envelope_fixture`,
  `test_spu_fidelity` and the others in `runtime/tests`).
- Do not read the oracle's source to settle a difference. A difference is
  settled by a fixture or stays marked "not observed" in the code.
