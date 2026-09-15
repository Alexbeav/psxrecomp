# Retained core correctness

This candidate restores two previously tested corrections absent from its
upstream base `89db8168dbb1da035aacd1d9631d5a3bb9469c1a`.

GTE correction `33e4b2820bafc06d731adc6719a3e5f17e822496` restores signed
44-bit intermediate arithmetic, instruction shift and clamp behavior, register
sign extension, RES1 storage and FLAG summary behavior. The register fixture
retains the original transfer checks and independent fixed arithmetic cases.
The expected bounds and staged flags follow the
[GTE specification](https://psx-spx.consoledev.net/geometrytransformationenginegte/).

MDEC correction `a09faa8a0` keeps Command Busy set while decoded output remains.
This preserves completion ordering in the runtime's buffered decoder. It does
not implement streaming decode timing. The public MMIO/DMA fixture checks input,
each output word, final drain, reset and snapshot restoration. Historical T33
RE2 validation motivated this rule; these synthetic checks are not a new RE2
gameplay or movie qualification.

Both fixes use existing guest register and snapshot storage. They add no option
or serialized section. Internal GTEState gains RES1; CPUState still stores it in
the existing `gte_data[23]`. Generated access helpers already cover the affected
registers. Full TAS/PGXP composition must separately bind its callback, codegen
and save identities.

The independent 4-bit output-size control exposed 64 bytes for a 32-byte mono
block. Existing correction `6a2588c60ab0e2ba95ae5f082d05fb7e6fc2b049` restores
rounded signed/unsigned packed nibbles. It is separate from Busy lifetime and
is not a new PGXP defect. A public 8-bit/4-bit comparison covers all 1024 signed
DC inputs and both output signs; it tests packing, not IDCT accuracy. The
[Beetle EncodeImage reference](https://github.com/libretro/beetle-psx-libretro/blob/master/mednafen/psx/mdec.c)
describes this rounded packing, while DuckStation uses its own decoder/output
convention. No new external implementation was imported.
