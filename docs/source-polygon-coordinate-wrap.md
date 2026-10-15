# Source polygon coordinate wrap

The optional source GPU profile now admits the complete sum of its signed
11-bit vertex and signed 11-bit drawing offset: -2048 through 2046. The old
post-offset11-bit guard rejected valid source commands, including Pepsiman's
partly offscreen GP02C packet after return 2288. Packet inputs stay intact.

Original gpu_polygon.cpp clips signed 11-bit Y and left X while retaining
unwrapped X/Y for color and UV interpolation. The shared walker now preserves
both X positions in the span callback. VRAM writes still use physical X and
nine-bit Y. Sprite callbacks retain their existing coordinates. Source
traversal order, oversize/degenerate rejection, other command admission,
clipping-area bound and lifecycle guards remain in place. Every sum in that
range is admitted; parsed 11-bit fields cannot produce another.

Documented rules. PSX-SPX "Vertex", Size Restriction: a polygon or line whose
vertices are more than 1023 apart in X or 511 in Y is not rendered, offscreen
parts included. PSX-SPX and No$PSX GP0(E5h): the offset is added to the signed
11-bit vertex and the result is truncated to signed 11 bits. The profile keeps
the raw sums for the size check and interpolation and wraps the stored row and
column (`source_poly_signed_coordinate`). The two agree whenever a primitive's
three sums lie in the same 2048-wide period, because truncation then moves every
vertex by the same multiple of 2048. They differ when the sums straddle a
-1024/+1024 boundary and the raw primitive is small enough to draw. Geometry 10
(X sums 1000, 1100, 1110) draws in stock 2.3; truncated first (1000, -948, -938)
it is 1948 wide and would be dropped. The fixtures are stock captures, so the
profile follows the oracle there, and the harness names the difference as an
edge. No hardware capture of that case exists in the programme yet.

ORACLE CORE: Octoshock 2.3 (BizHawk tag 2.3, commit
a15b31a46bdac27d843d3ebbc5a860012d8452fb). This is oracle compatibility. The
polygon span helper was rewritten from measured behaviour in T172
(`runtime/tests/gpu_polygon_provenance.json`).

The authored CPU/MMIO fixture covers 12 ordinary, offscreen, wrapping and sign
boundary geometries, all 3 texture depths, separate/overlapping texture data,
and flat versus Gouraud+dither rendering. Two draws retain the cache. All 144
complete final 1 MiB VRAM hashes match stock 2.3. A leaf observer with the same 144
images records 288 command-work values; native O0/O2 match all of them. Inputs
are authored ROM/RAM programs with a completion marker, not retail data or
emulator state. The previously rejected coordinate case now has an admitted
one-pixel work assertion. Full Pepsiman, native ending and unchanged Tekken
regressions remain independent gates.

Before the 144 cases the harness prints 30 documented edge checks: triangles
and lines 1023 wide or 511 tall drawn, 1024 or 512 dropped (also partly
offscreen), the line charge at those sizes, the four ends of the -2048..2046
sum range, the nine stock geometries whose sums wrap alike, the two that
straddle in Y and draw under neither rule (7, 8), and geometry 10. The
contracts script requires all 30 and all 144.

Run CTest -R tas_gpu_polygon_wrap (GNU compiler builds) for both.
