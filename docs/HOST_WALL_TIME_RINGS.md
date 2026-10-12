# Always-on host wall-time rings (hitch attribution)

A gameplay hitch is wall time the emulation thread did not spend running the guest. These rings
record where it went, continuously from process start in every build, so a probe never arms
anything: it queries a window that already happened.

| Ring | TCP command | Holds |
|------|-------------|-------|
| frame stamps | `frame_wall_ring {"from":SEQ,"n":N}` | one `[guest frame, wall us, interpreter insns (low 32 bits)]` per guest frame, 131072 frames (~36 min at 60 Hz) |
| spans | `host_span_ring {"from":SEQ,"n":N}` | `[seq, kind, t0_us, dur_us, frame, emu, arg]` for every named blocking-capable operation of 50 us or more, 65536 spans |
| host CPU sampler (debug-tools builds) | `host_profile {"frame_lo":A,"frame_hi":B}` | 1 kHz instruction-pointer samples of the emulation thread, 1 Mi samples (~17 min), with per-module totals for samples outside the exe |

Wall time is microseconds since the first ring call. `emu` is 1 for the emulation thread, 0 for a
worker (compiler child, publication preparer).

Span kinds (`runtime/include/host_span_ring.h`): `autocapture_tick`, `provider_poll`,
`publish_commit`, `loader_rescan`, `dll_register`, `lazy_load` (a synchronous first map of a shard
at a dispatch miss), `capture_on_dma`, `capture_before_dma`, `ac_request` (spawning the compiler),
`ac_done_join`, `ac_prepare_image` (worker), `ac_child` (compiler lifetime, worker).

Reading a hitch: per frame, `wall delta - sum(emulation-thread spans overlapping the frame)` is
the time no span explains; then ask the host sampler for that frame window. A title's report tool
joins them (`LegendOfDragoonRecomp/tools/lod_hitch_report.py`).

Findings the rings produced (LoD, 2026-10-12): a toolchain staged from another emitter than the
runtime wrote every shard where the loader never looks (now refused loudly, see
`autocompile_check_toolchain_hash`), and shards compiled during play were mapped by the emulation
thread because `compile_overlays.py` never announced them (now `PSX_SHARD_PUBLISHED`, see
`announce_published_shard`).
