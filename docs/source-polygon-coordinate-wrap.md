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
clipping-area bound and lifecycle guards remain in place. Values outside the
possible vertex-plus-offset range remain rejected.

CLAIM / DERIVED-FROM: original Octoshock 2.3 DrawTriangle / DrawSpan,
upstream a15b31a46bdac27d843d3ebbc5a860012d8452fb. This is source compatibility.

The authored CPU/MMIO fixture covers 12 ordinary, offscreen, wrapping and sign
boundary geometries, all 3 texture depths, separate/overlapping texture data,
and flat versus Gouraud+dither rendering. Two draws retain the cache. All 144
complete final 1 MiB VRAM hashes match stock 2.3. A leaf observer with the same 144
images records 288 command-work values; native O0/O2 match all of them. Inputs
are authored ROM/RAM programs with a completion marker, not retail data or
emulator state. The previously rejected coordinate case now has an admitted
one-pixel work assertion. Full Pepsiman, native ending and unchanged Tekken
regressions remain independent gates.

Run CTest -R tas_gpu_polygon_wrap for the native golden comparisons.
