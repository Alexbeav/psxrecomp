# Native CPU spans: continuation evidence

This adds bounded findings to the [original measurements](native-cpu-spans-20261028.md).
It does not change the historical samples, acceptance policy, default-OFF
selection or experimental qualification status.

## Retained Tomba restore observations

All 32 original 2 MiB RAM captures were rehashed, without relinking or launching
the game again. The captured PCB/thread-array metadata and the pinned
[OpenBIOS thread ABI](https://github.com/pcsx-redux/pcsx-redux/blob/55fbf0468345acfbe5512628a3be5c0cfe3f22e7/src/mips/common/kernel/threads.h)
identify every sampled differing word as volatile register context in allocated,
inactive Thread 1. Its saved return PC, stack, return address and callee-saved
registers match. The candidate-only two-word difference is AT/A2; both are
defined before first use on the captured controller path, and full RAM converges
at the next original sample. Independent review verified the captures, ABI and
actual generated instruction sequence.

That resolves the previously unclassified candidate-extra live-in concern.
It does not explain every other volatile difference or establish full-state
equality, a harmless general cause or two-peer compatibility. The shared
strict-digest/current-build caveat is tracked separately as `beads-eio.3.424`.
No bytes were masked and no product memory writes were introduced.

## Finite Ape confirmation attempt

The unchanged original package was tested on one Windows host with a hard
0.25 physical-core-equivalent Job cap applying to the observer, game and
telemetry children. Both choices retain PGXP/9x OpenGL, batch mode 0,
interpolation OFF and an empty rewind ring. This constrained cohort is separate
from the original unrestricted measurements.

The complete control/native/native/control block observed 20.26% lower CPU per
guest frame and 19.76% lower process cycles per frame. Every run fails both
predeclared host limits: mean load was 74.42–82.27%, and the fraction of sampled
intervals over 90% was 6.67–23.26%. These results are supporting only and do not
close repeatability qualification. A reverse block completed one native run,
then stopped when the next control observer's required GPU query timed out.
The incomplete block has no A/B result. All five complete launches and the
failed control launch closed their owned games with exit code 0.

[Machine-readable per-game inputs](native-cpu-spans-continuation-20261029.json)
retain identities, per-launch metrics, exclusions and the failed-cohort scope.
Original raw samples, GPU telemetry, settings and failure/close receipts remain
with campaign `beads-wuhs`. No unrestricted FPS, physical-speaker or GPU benefit
is claimed. Native in-game card completion, common-profile qualification and
final owner adoption remain open.
