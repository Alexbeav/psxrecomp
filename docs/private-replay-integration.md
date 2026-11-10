# Preliminary Pin I v0.1 composition

This is a private, unshipped integration candidate. It combines the immutable
standalone `psx-replay-final-20261105` source e841503c with prepared modern core
f96977c2 and the separately retained UI source0dc6fb75. It is not another mint,
a public release, or a claim that the modern composition passed the replay fleet.
The exact candidate tree, later checks and portable source/UI identities belong
in the preliminary packet's manifest; historical pin records remain immutable.

## State layout

The complete boot-state writer uses version17 without trusted host providers,
or18 when the trusted host state section is present. The read floor is17.
Replay16 and modern9/10 are incompatible and rejected at the header. No
cross-version or cross-product restore is qualified by this composition.

Each machine field has one owner. The complete CPU section owns load values and
stall deadlines. The CPU execution continuation holds default interpreter host
pipeline values only when the source CPU profile is inactive. GAME_START owns
the BIOS-to-game latch, SPUCD owns queued CD audio, and trusted MOD_HOST retains
its producer/cookie rules. Source raster/GPU/timer/DMA/IRQ sections remain
profile-scoped. Instruction-cache serialization is not a new cache-restoration
qualification; the previous runtime limits remain in force.

The CD codec retains the modern CDF4 default prefix and appends SCD2 for optional
source state. The source FIFO and staged tray notice are serialized once.
Configuration and an exact saved tape prefix are checked before mutation;
the loaded extended tape identity is retained. See [source CD profiles](source-cd-profile.md).

The SIO codec retains its source ACK/card/DTR/MODE/IRQ-sequence state alongside
modern JogCon/NeGcon devices. Modern steering, motor and physical-device bytes
extend the replay prefix. The mode-lock field is emitted once: its source
prefix owner when that profile has one, otherwise its modern extension owner.
The existing immutable transaction/legacy-wire golden remains an explicit
projection, while the full composed layout requires its own round-trip checks.

The SPU owns guest-cycle sample production. The frontend drains that output;
it does not also consume a second legacy sample budget. Source decoder/reverb
state and modern audio/overlay controls remain distinct. This source design
alone does not prove listening, playback or per-title timing.

## Routing and prepared work

Modern player UI and netplay use stem-keyed disc maps. Source route events and
program-disc membership use exact ordered metadata and original asset digests.
Admission, transactional event receipts, original cards and observer state stay
content-bound. Two-pad routes retain their explicit no-resume limit.
The approved program-set parser rejects duplicated/out-of-range set positions;
absent program_discs keeps the every-disc default.

The modern module/overlay/provider path remains the UI owner. Retired player
look/audio settings are not silently restored as parallel controls; existing
title baselines and migration tests remain. The older five approved prepared
rows already in e841 stay included; conditional row412 remains deferred.
Extracted TinyCC cache binaries/SDK/docs are local recreated dependencies,
not new source content. Their older history is retained privately.

## Qualification

The standalone replay packet keeps its exact measured scope: 22 prior c03 cold
passes, three gap-free c03 resumed completions, and Exoddus cold e841. This is
23 cold and three resumed outcomes on their recorded products/providers,
not26 cold runs on this modern tree. Public PR packets remain separate clean
patches; private source-derived runtime changes are not public-ready patches.

The combined source is not yet GCC-qualified. Python parsing proves syntax only.
The required combined Eagle GCC runtime/recompiler checks and meaningful
source/controller/module regressions must bind the final candidate tree.
They require a host/resource window compatible with the current allocation.
No new game round, formal product release gate or public submission is implied.
