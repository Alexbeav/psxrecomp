# Optional MDEC source profile

`PSX_MDEC_SOURCE_MODEL=octoshock-2.3` selects the maintained authored FIFO
controller, with 32-word input/output queues and 474 clocks per completed block.
`nymashock-1.29.0` and `nymashock-1.32.1` select the same bounded controller
with 512 clocks per block. Empty/unset selects the modern default decoder;
unknown names fail explicitly. These are source comparison models, not measured
PlayStation hardware timing. No setting is enabled by default.

The controller comes from our accepted `ac56b624` implementation (PS1B-425).
Its contract is the corpus `MDEC-SOURCE-PROFILE-FIFO-SPEC.md`; independent stock
observation limits are in `MDEC-FIFO-STOCK-CAPTURE-F1-F5-READING.md`. The scalar
arithmetic comes from our PS1B-188 rewrite against PSX-SPX and authored M-series
fixtures. The profile header, authored inputs, hashes and fixture provenance
are retained. No reference emulator implementation is linked or copied here.

The MDEC-only optional DMA path covers channels 0 and 1. It feeds MDEC before those
channels at guest service boundaries, samples readiness at block starts, uses
64 startup clocks, discards positive unused allowance after service, retains
negative debt and applies nonlinear color output addresses. Completion flags
latch independently of master IRQ output. Modern RAM geometry still folds DMA
addresses. Other channels keep their modern implementations unless their explicit source
bus selectors are enabled. The [source bus profile](source-bus-profile.md) then
shares service/arbitration with MDEC; it does not advance the decoder twice. The exact
service entry point in `dma.h` is for authored transcripts whose grid phase is
unknown; normal guest service uses the 128-clock grid.

The default decoder retains its SIMD paths, busy state, media-job selection,
activity age, capacity checks and snapshot prefix. The optional path uses its
separate scalar decoder and updates the same color-activity diagnostics.
An enabled MDEC snapshot appends `SMF1` (913 bytes), including model cost,
queues, pending encoded/decoded pixels, table positions and chroma blocks.
DMA appends `SMD1` (56 bytes), including service clock, pending block progress,
address and signed allowance for both channels. Optional tails are validated
before the section is applied. Restore requires an enabled profile with the same block cost; both 512-clock
selectors share that wire contract;
legacy stage optional snapshots are not promised compatible. Default wire
sizes and the existing default reader behavior remain unchanged.

`mdec_source_pipeline_O0_test` / `O2_test` check 48 complete authored
transcripts cold and with a snapshot after every operation.
`mdec_source_dma_O0_test` / `O2_test` check 24 transcripts the same way:
every guest-visible field must match; only internal fields permitted by
SPEC-PS1B-186 amendment 4 may differ. `mdec_source_fifo_rows_test` checks 518
controller assertions. Snapshot tests check malformed-tail refusal without
mutation, both model costs, activity and grid/exact-service agreement.
[Clean decode fixtures](testing/MDEC_CLEAN_DECODE.md) cover arithmetic separately.
These component tests do not qualify retail movies, title routes, audio,
full-game behavior, hardware timing, strict replay or external-oracle replays.
