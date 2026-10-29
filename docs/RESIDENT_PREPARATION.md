# Shared resident preparation ownership

This experimental package changes the complete shared cold preparation job;
there is no measured candidate runtime benefit yet. Existing Tomba diagnostics
observed 673.7 ms for an empty application pack and 56.7 ms for its verified
pack. Those figures select a useful cost boundary, not removable hashing time.

`PSX_NATIVE_RESIDENT_PREPARATION` selects owned blobs in ENHANCED builds.
Explicit OFF and REFERENCE retain the original flat builder. A normal caller
receives the same immutable file/derived bytes, metadata and dedup aliases.
The native builder hashes source bytes once for stock checks and dedup, moves
new source allocations into the pack, owns borrowed derived outputs once, and
publishes the original aligned pack format directly from these owned blobs.
It avoids repeated growth/copying of the complete resident payload. Warm cache
loading still verifies every blob and uses the original flat allocation.

Every file is still read and every derive callback still runs in spec order,
including duplicate source files. Source pointers remain valid while callbacks
emit further blobs. Modified-disc policy, opaque callback behavior/errors,
fingerprint, blob order, table digests, atomic file replacement and memory-only
fallback are preserved. No title loader or codec changes are included. The
portable SHA implementation remains available on every supported platform.

This is host preparation only. Selection keeps simulation identity unchanged;
qualification requires byte-identical entire packs and caller observations.
The real original/native whole-job fixture covers stock/modified inputs,
duplicates, derived outputs, callback/source failures, invalid input, cache
corruption/rebuild and publication failure. The existing real ISO/mod-runtime
fixture also runs the owned implementation. These gates do not establish a
built-game speed or memory gain; matched candidate startup/runtime evidence
and the campaign's remaining adoption checks are separate.
