# Optional source SPU profile

The SPU selects this maintained authored profile at initialization when
`PSX_GPU_DMA_MODEL` is `octoshock-2.2.2-bounded-linked-list` or
`octoshock-2.2.2-bounded-quad`. Without that explicit selector the modern
runtime's decoder, sweeps, key behavior, private voices and snapshot wire
remain unchanged.

The optional hardware-voice path retains pending key masks, four sample
start delays, per-word ADPCM decode queues, pitch modulation, loop-copy
disarm, source envelope/sweep ticks and sample-boundary register readback.
Hardware voices bound to immutable enhancement banks use those banks with
this decoder; bank fetches do not raise hardware RAM-fetch interrupts.
The 24 private enhancement voices retain the modern decoder and lifetime.
Guest-clock catch-up, the output queue, output suppression, PCM overlay and
48-voice shadow layout remain owned by the modern runtime.

An enabled snapshot appends the accepted 1,716-byte `SPK2` payload after the
modern hardware and optional private records and before the existing
four-byte sample phase. It includes both pending masks, 24 delays and all
24 complete decoder queues. The reader checks its marker and field bounds
before changing guest state. The phase still anchors immediately to the
restored guest clock. The current separate CD-input snapshot remains in
use; the older absolute clock and duplicate CD ring are not appended.
Optional and ordinary snapshots are profile-specific. Historical stage
snapshots have a different base voice wire and clock/CD layout and are not
claimed compatible.

The authored code and fixture provenance is retained in
[spu_envelope.h](../runtime/include/spu_envelope.h),
[spu_adpcm_sample.h](../runtime/include/spu_adpcm_sample.h) and
[spu_adpcm_provenance.json](../runtime/tests/spu_adpcm_provenance.json).
The behavior specification is the private corpus reference
`references/ps1/SPU-SOURCE-PROFILE-VOICE-SPEC.md`; its kept/unobserved rows
remain bounded by that specification. No reference emulator implementation
was used for this integration.

The regular runtime suite registers the source key latency, voice rows,
S1 block flags, S4 source readback, envelope fixtures, ADPCM arithmetic and
source snapshot integration tests. The S4 imported expectations are for the
optional profile; modern default behavior retains its existing tests.
`tools/upstream_pgo_verify.ps1 -Phase spu-source-profile -Attempt <unused>`
also compiles and runs them at O0 and O2. `-IncludeTests` accepts comma-separated
fixture names (`key_latency`, `source_voice_rows`, `source_snapshot`,
`register_readback`, `s1_block_flags`, `adpcm_sample`, `envelope_fixture`).
Unknown selections fail. The snapshot integration uses both selectors and
checks pending keys, queue state, nonzero clock phase, CD input, RAM capture,
private voices and bank bindings through 16 continuation points. These are
authored fixture results, not new hardware audio parity, listening tests,
external-oracle comparisons or strict replay qualification. The existing
disabled default fidelity test remains disabled for its separately recorded
sweep failures.

The [optional source bus](source-bus-profile.md) separately preserves SPU DMA
transfer service and shared CPU ownership. Its GPU DMA selectors also select
the source SPU transfer schedule; only linked-list/quad selectors select the
voice/key timing described here. Modern private voices, bank bindings and
restored-clock anchoring remain part of the maintained SPU implementation.
