# Cold Octoshock 2.3 MDEC comparison profile

`PSX_MDEC_SOURCE_MODEL=octoshock-2.3` selects an explicit source-core
compatibility model. The default runtime retains its existing MDEC scheduling.
The Pepsiman adapter selects the new profile; the original movie is unchanged.

CLAIM: the original source advances MDEC decode work and both DMA channels at
the common DMA service boundary. A fixed output-word delay cannot express its
32-word FIFO backpressure, 474-clock completed-block work, positive-credit
boundary, or non-linear color output addresses. The new FIFO controller feeds
the existing native RLE/IDCT/color arithmetic one complete block at a time.
GPU service precedes MDEC, then DMA0 and DMA1 precede the other channels. DMA
request readiness is sampled at block starts, with 64 startup clocks and one
clock per word. Enabled completion flags latch independently of master IRQ
output. No CPU halt or new per-word bus timing is introduced for these channels.

DERIVED-FROM: upstream BizHawk tag 2.3, commit
`a15b31a46bdac27d843d3ebbc5a860012d8452fb`, specifically
[mdec.cpp](https://github.com/TASEmulators/BizHawk/blob/2.3/psx/octoshock/psx/mdec.cpp)
and [dma.cpp](https://github.com/TASEmulators/BizHawk/blob/2.3/psx/octoshock/psx/dma.cpp).
This is source compatibility, not measured PlayStation hardware timing.

ORACLE: `runtime/tests/mdec_source_pipeline_fixtures.json` binds 48 isolated
source MDEC transcripts (zero and cosine matrices, DC/AC input, 15/24-bit,
signed output, bit15, clock cadences 1/127/128, backpressure and reset).
`mdec_source_dma_fixtures.json` binds 24 production DMA/MDEC transcripts
(both startup orders, master IRQ initially enabled/disabled, partial write
service, channel acknowledgments, complete output and sentinel reads).
Every operation is authored; none contains BIOS, game or movie data. Inputs
are losslessly compressed little-endian words and individually SHA-256 bound.
Expected hashes cover every declared scalar/output byte after every operation.
The external oracle source heads, trees and DLL hashes are in those files.
The isolated oracle adds only test entry points around original functions and
state. It is an active unit harness, never a movie replay core, and is never
linked into the runtime. The original stock DLL independently matched the
existing completed decode bytes in 16 DC/AC cases at O0 and O2.

REPRODUCE: configure the existing runtime CMake test build with GNU C and run
`ctest --test-dir <build> -R mdec_source_ --output-on-failure`.
Both O0 and O2 variants replay the complete oracle transcripts. DMA variants
also reject unsupported block sizes, direction/step modes, active register
replacement, DPCR changes and state capture. The transcript driver protocol
is defined in `test_mdec_source_pipeline.c` and `test_mdec_source_dma.c`.

BINDS: all golden cases carry source build, input and expected-state hashes.
The authoring recipes and external-oracle recovery bundle are retained in
the T52 campaign recovery checkpoint, separate from the MIT runtime source.

Scope: cold source scheduler, enabled forward request DMA with 32-word blocks,
and 15/24-bit color decoding. Unsupported modes fail explicitly. Save-state
capture/restore, monochrome output, cancellation mid-block and hardware timing
are not qualified by these tests. Full Pepsiman movie parity, native ending
and the registered Tekken route remain separate acceptance gates.
