# HD FMV guest timeline contract

The optional `fmv_hd/` decoder replaces a 24-bit scanout only when the scanout
signature identifies a pack source frame. The presenter receives an immutable
`FmvHdTimeline` captured with that native scanout on the guest thread: guest
cycle, guest frame and execution epoch. Movie identity is the matched pack
movie plus source-frame signature. An unmatched scanout or failed decode uses
the native frame.

Within one recognized source frame, interpolated HD subframes advance from
guest cycles at 33,868,800 cycles/second. Repeated presents while the guest is
paused keep the same subframe. A new recognized source frame starts at
subframe zero. Savestate and rollback loads increment the execution epoch;
backward guest cycles or frame numbers also reset recognition. A non-movie
scanout ends recognition, including a skip or movie end. Restart/context
recreation releases the decoder and all tracking.

The synthetic native test covers malformed packs, decode/worker failures,
restart, paused host time, guest-cycle advancement, a load epoch, and unmatched
scanout fallback. Windows and Mac runtime builds and the focused test pass.

Upstream review still needs a consuming-title replay with a pack built from
that title's own movie, covering pause, skip, seek, state load, rewind and
rollback where supported, with captured guest-cycle/frame/epoch, matched
movie/source/subframe, and synchronized audio evidence. R4's current public
candidate has no `fmv_hd/` pack and does not pin this optional feature, so it
cannot supply that evidence. The scanout signature alone also cannot
disambiguate two distinct source frames with identical images; title replay
must verify that ambiguity does not affect the targeted movies.
