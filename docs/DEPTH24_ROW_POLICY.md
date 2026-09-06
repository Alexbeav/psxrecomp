# Private movie row presentation policy

This candidate reapplies the presentation behavior from commits
`1f02ca1855b6f351eaef31f5bfe222860f41f74f` and
`5c77c40550b66153fcfb9ede75b51d9bd4d332fc` to frozen base `63446a28`.
Only context changed: the current base has no `gpu_vblank_period_refresh` calls
at the display-mode and snapshot-parser locations. Timing remains unchanged.

`gpu_depth24_present_row` presents unwritten rows as black. Completed CPU-to-VRAM
rectangles mark their rows, including physical wrap. A depth transition clears
coverage. The first presented row resets coverage when MDEC becomes active
within the existing ten-frame window. Snapshot parsing marks all rows valid.
Guest VRAM and decoded pixel bytes remain unchanged.

The exact prior policy was first reproduced at `08b15cd3`. It hid Colony Wars'
preuploaded loading still for its entire visible interval. The candidate adds
one guard: row masking requires active MDEC streaming. This preserves static
24-bit images and retains the prior movie-band behavior. This remains a
presentation policy, not a hardware invariant. It does not serialize row coverage or streaming history.
Vulkan parity and snapshot presentation remain separate qualification gates.
The private Colony Wars report owns retail evidence and operator acceptance.

`runtime/tests/test_depth24_row_policy.c` exercises the actual upload producer,
display-mode transition and row presenter with synthetic RGB bytes. Compile it
with GCC, the runtime include directory, `-O2 -flto -fwhole-program`,
`-ffunction-sections -fdata-sections`, and `-Wl,--gc-sections`.
It needs no game or BIOS data. Whole-program optimization removes unused GPU
entry points and their external services from this focused executable.
