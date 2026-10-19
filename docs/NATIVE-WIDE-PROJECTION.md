# Native-wide projection correction

Trusted title plugins can call
`psx_mod_set_native_wide_projection_correction(1)` to recover horizontally
saturated GTE vertices during native-wide rendering. The default is disabled.
It is independent of the user-facing geometry and perspective-texture options.

The PS1 clamps projected screen X to -1024 through 1023. Two different vertices
can therefore collapse onto one X edge, folding a planar quad when a wider view
reveals its triangles. The renderer recovers the original horizontal projection
from the existing PGXP transport only when the packet address and complete word
match, depth is present, the integer Y still agrees, and X actually extends past
the corresponding saturation edge. Missing or stale provenance stays stock.
The bounded projection transport's own +/-4096 endpoints are rejected.

This changes host triangle positions only. Guest SXY registers, packet words,
vertical coordinates, NCLIP, collision and ordering-table behavior stay intact.
With zero native-wide reveal, including a selected 4:3 view, correction is
inactive. Existing geometry correction can still supply subpixel coordinates
for ordinary vertices when the user enables it separately.

`ws_nw projection_correction=0|1` provides a diagnostic A/B toggle. The reply
reports the selected policy and the cumulative corrected vertex count, reset
on a policy change. It does not toggle geometry or texture correction.

The executable GPU regression uses a captured folding GT4 shape, verifies both
triangle windings after recovery, and checks unchanged packet/Y data, both
saturation edges, zero reveal, disabling the policy, missing depth, stale words,
wrong addresses and transport endpoints. The PGXP regression separately verifies
that a saturated MFC2/store retains its exact extended projection while the
ordinary conservative geometry consumer continues to reject it.

This policy cannot recover a projection whose provenance has already been lost
through unsupported CPU arithmetic, or repair a guest culling decision made
before GPU submission. Those paths still need title-specific qualification.

## Saturation-aware winding consumers

A title can separately bind exact branch words with
`psx_mod_set_native_wide_nclip_sites(addresses, expected, count)` and list its
BLEZ, BGTZ or BGEZ consumers under `[widescreen.cull] nclip_exact_sites`.
Enable `[widescreen] precise_nclip = true` to collect checked GTE projections.
No sites are bound by default. Architectural NCLIP MAC0 and FLAG remain stock;
only the selected rendering branch consumes the corrected winding.

This narrower predicate requires a matching native MAC0, live projection
generation, matching packed words and integer Y, positive depth outside the
GTE near region, bounded precision coordinates, and an X projection that
exceeds its corresponding saturation rail. It leaves ordinary subpixel winding
unchanged. Zero reveal, stale/missing precision, altered branch words, other
addresses, speculative execution and timeline invalidation retain the native
decision. Both triangle rejection and the quad's winding keep branches need
to be identified from the title's renderer.

Generated functions, cached overlay callbacks and the interpreter use the same
full-word-bound host predicate. Overlay callback ABI v27 and codegen version 14
prevent older cached modules from hiding the new forwarding surface.
The `ws_nw` diagnostic also reports `nclip_rescues`, the number of branch
decisions corrected since the title last bound its sites.
