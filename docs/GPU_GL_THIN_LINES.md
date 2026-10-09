# Upscaled one-pixel textured lines

An affine textured triangle can encode a one-pixel row or column. Two vertices
share the minimum coordinate on its short axis. The third is one pixel across,
at one end, and repeats that end's texture coordinates and colour. A column
requires the extra vertex at its top; a bottom tip leaves the first native row
empty and keeps the original path. The native
software rasterizer covers the whole strip. Above 1x, drawing its geometric
diagonal leaves old subpixels in the persistent OpenGL target.

The OpenGL backend expands that line encoding into two rectangle triangles at
higher scales. Both use the original endpoint attributes along the long axis.
The existing texture window, clipping, mask, blend, batch and transfer paths
still apply. There is no frame clear or title setting. Native 1x rendering,
rectangles, paired triangles from quad commands, cross-axis texture or colour
changes, subpixel positions and perspective overrides keep their triangle path.
The GPU exposes quad provenance only during a complete GP0 command dispatch;
it clears that transient flag before returning.

`runtime/tests/test_gl_thin_triangles.c` creates a hidden real GL context and
compares every upscaled pixel of authored rows and columns against an independent
native software footprint. It reads the source before and after a VRAM copy.
Old-colour sentinel and persistent-background controls detect stale pixels and
unjustified clears. Shaded, semi-transparent, masked and paired-quad cases check
both paths. Fingerprints of cross-axis UV, precision and perspective cases can
be compared against the same fixture on the old renderer. These fingerprints
are preservation checks, not independent raster accuracy claims.

The MinGW hardware-test dependency checks are shared with
[the readback test](GPU_GL_READBACK_REGIONS.md). With those dependencies present,
`gl_thin_triangles_test` runs at 1x, 2x and 4x. The same fixture can be selected
with `run_gl_readback_region.py --fixture <fixture> --scales 1 2 4`; the runner
keeps its existing 1x/4x default for the readback fixture. Missing dependencies
are reported at configuration and do not establish a hardware pass.

This source regression does not qualify Duke Nukem or an SDL3 title product.
The real rooftop, matching source/product/input route, independent review and
release gates remain separate.