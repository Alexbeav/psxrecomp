# Optional source bus profile

The optional source comparison controller preserves the accepted authored
`ac56b624` service contract beside the modern default controllers. It does not
model measured PlayStation bus timing. No selector or build option is enabled
by default. The retained provenance is in the projection headers,
`runtime/tests/service_clock_provenance.json` and
`runtime/tests/raster_clock_provenance.json`; the original contracts are
SPEC-PS1B-186-DMA-BEHAVIOUR-v2 and GPU-SOURCE-PROFILE-RASTER-SPEC.

## Selection

All nonempty source DMA selectors require `PSX_INPUT_ROUTE_FILE` to be present.

| Variable | Accepted value | Controller |
|---|---|---|
| `PSX_GPU_DMA_MODEL` | `octoshock-2.2.2-vram-upload` | Source GPU block DMA |
| `PSX_GPU_DMA_MODEL` | `octoshock-2.2.2-bounded-linked-list` | Block and bounded linked-list DMA |
| `PSX_GPU_DMA_MODEL` | `octoshock-2.2.2-bounded-quad` | Full GPU command/raster service and CPU bus owner |
| `PSX_CD_DMA_MODEL` | `octoshock-2.2.2` | Source channel 3 DMA |
| `PSX_INPUT_ROUTE_DMA_MODEL` | `octoshock-2.2.2-otc` | Source channel 6 DMA |

GPU source DMA also selects the source SPU transfer schedule. SPU voice timing
keeps its existing linked-list/quad selectors; see [source-spu-profile.md](source-spu-profile.md).
CD command, receive, drive and CDDA selectors remain separate; see
[source-cd-profile.md](source-cd-profile.md). MDEC source DMA joins the same
service owner when its existing selector is enabled; see
[source-mdec-profile.md](source-mdec-profile.md).

The full quad selector additionally requires
`PSX_INPUT_ROUTE_FIELD_MODEL=octoshock-2.2.2-ntsc-raster`, a diagnostic build with
`PSX_SOURCE_BUS_BOUNDARY=ON`, matching regenerated game/BIOS source, and
`PSX_NATIVE_CPU_SPANS=OFF`. This is an explicit developer comparison build;
ordinary ENHANCED builds keep native spans. The full renderer is software at
native resolution with geometry enhancement disabled. Unsupported source
commands or conflicting rendering state fail explicitly. Initialization is
cold-only. `PSX_GPU_STATUS_MODEL=octoshock-2.2.2-raster` separately selects the
source raster GPUSTAT field bits.

For a bounded comparison run, use the existing per-process rendering overrides
`PSX_INTERNAL_RESOLUTION=native`, `PSX_SUPERSAMPLE=1`, `PSX_AA_MODE=off` and
`PSX_DYNRES=0`, with the software backend. They isolate the required 1x path
from inherited player settings. A software session that requests 4x is outside
this source raster contract and is refused when the source draw executes.
Do not change player defaults to make a source comparison run.

Source builds use overlay flavor bit 4. Both generated overlays and loadable
BIOS modules must carry that bit and compile with the instruction boundary
enabled. The default overlay ABI stays at 35 and BIOS-module ABI stays at 2;
the flavor prevents incompatible callback tables or boundary-free modules
from crossing between builds. DLL boundaries publish their local cycle
charges before calling the host. This does not qualify a particular title's
dynamic-module route or another platform's compiler.

## Service and ownership

GPU work precedes DMA at a service point. Shared DMA order is channels
0, 1, 2 block/list, 3, 4 and 6. Transfers start with 64 clocks of allowance,
service on the 128-clock grid, drop unused positive allowance and retain
negative debt. GPU block overhead and the first word occupy the same step.
Readiness is sampled at block starts. Source DPCR/CHCR/DICR handling retains
accepted kick, stop, mirror, completion and IRQ-edge behaviour. Source RAM
access uses the modern live RAM geometry.

The full GPU service owns command queue, split drawing, raster progress and
renderer texture history. Its CPU instruction boundary publishes read stalls
and waits without retiring an opcode while DMA owns the bus. The accepted
COP2 bypass remains explicit. Default per-word GPU lists, media jobs, geometry,
render-pass paths, private banks and enhancement settings keep their existing
implementations. A source profile does not qualify those optional combinations.

## State

The modern default DMA/GPU prefixes remain unchanged. MDEC-only `SMD1` remains
56 bytes. A selected source bus adds DMA `SDB1` (308 bytes), including exact
selector flags, RAM geometry, all seven machines, allowance/debt, progress,
service stamps, halt state and published/live read waits. Full GPU selection
adds `GSQ1` queue/provenance state, `SGR1` service/command/two-raster state,
software texture cache/palette state, and selected `ISR1` interrupt-raster state.
`GSQ1` is 236 bytes, `SGR1` is 552 bytes, the texture history is 4624 bytes,
and `ISR1` is 92 bytes. With all selected, the modern 288-byte GPU prefix
therefore occupies 5792 bytes including its optional tail.
The comparison clock is restored exactly; modern SPU sample-clock anchoring is
retained. Malformed or mismatched optional state is rejected before applying it.

Legacy stage anchors used different GPU/DMA prefixes and separate DMA/service/
raster sections. They are not wire-compatible with these native extensions.
Older native source anchors that lack the required new tails are rejected;
no required tail is silently discarded. Ordinary default anchors retain their
existing wire layout and normal identity checks.

## Checks and limits

Maintained authored tests cover source service/debt/drop/register edges, active
GPU/CD-endpoint/SPU/MDEC/OTC operations, independent software raster rows,
malformed/config-mismatched state, and per-operation snapshot continuation at
O0/O2. The combined device fixture declares an authored CD sector endpoint;
real CD command timing remains covered by the separate CD transcript suite.
The generated CPU test executes an authored MIPS function through emitted C.
None of these checks is title gameplay, listening, platform/rendering parity,
an external-oracle replay, or fleet qualification. Bounded consumer evidence
and remaining limitations belong in the private evaluation report.

The private bounded-player verifier records the selected source environment
keys and executable SHA-256 in its JSON sidecar. That wrapper provenance does
not fill older replay-header metadata fields or change replay qualification.
