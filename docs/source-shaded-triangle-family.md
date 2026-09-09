# Source shaded-triangle family

Pepsiman qualification 16 clears the shaded-quad admission boundary and
matches all 5269 captured source RAM/clock returns. It then stops when the
source-profile table rejects GP0 31 at clock 2982504064. The diagnostic's
draw mode 0218 is context, not an established draw-mode defect.

The renderer and work projector already implement the shared shaded-triangle
flags. This correction admits opcodes 31/32/33 alongside 30. The low bit is
ignored for these untextured commands; bit 1 selects semitransparency. Packet
length, command dispatch, work calculation and rasterization are unchanged.
Other family and lifecycle guards remain in place.

An authored CPU/MMIO fixture covers all four opcodes, four blend modes, four
mask modes and dithering off/on. Its 128 complete final VRAM images match
stock Octoshock 2.3; the passive observer preserves each image and provides
128 command-work values. Both native O0/O2 builds match every image and work
value. All 64 ignored-bit source alias pairs agree. Before correction the
native fixture stops at opcode 31 after the 32 opcode 30 controls.

The fixture manifest binds source lineage a15b31a, stock core, observer and
authored driver. No retail data or emulator states are included. Run CTest
-R tas_gpu_shaded_triangle. Full unchanged Pepsiman and Tekken regressions
remain separate acceptance gates; this is source compatibility evidence,
not a whole-game or PS1 hardware accuracy claim.
