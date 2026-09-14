# Source shaded-quad aliases

After the recorded-sector SubQ correction, both Pepsiman native attempts match
all 5,261 captured source RAM/clock returns. The next return is missing because
the source-profile command admission table rejects GP0 0x39. Its diagnostic
also prints draw mode 0x0215; that value is context, not the identified defect.

The native renderer already implements shaded quads by their shared command
flags. For these untextured commands, bit 0 does not affect rendering or work.
The source profile now admits 0x39 and 0x3B alongside 0x38 and 0x3A. Packet
length, first/second triangle dispatch, work calculation and renderer are
unchanged. Other command-family and lifecycle guards remain in place.

An authored CPU/MMIO fixture covers all four shaded-quad opcodes, four blend
modes, four mask modes and dithering off/on. Its 128 complete final VRAM images
match stock Octoshock 2.3. The passive command observer preserves those images
and records 256 work values, with declared waits restoring work credit between
quad halves. All native O0/O2 images and work values match. Every ignored-bit
source alias also matches its admitted counterpart. Before correction, the
native fixture stops at opcode 0x39 after the 32 opcode 0x38 controls.

Source lineage is a15b31a46bdac27d843d3ebbc5a860012d8452fb; the fixture manifest
binds the stock binary, passive observer and authored driver. No retail data or
emulator states are included. Run CTest -R tas_gpu_shaded_quad. Full Pepsiman
and unchanged Tekken regressions remain independent acceptance gates.
