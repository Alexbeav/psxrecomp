# OpenGL native readback regions

OpenGL owns the rendered video memory in 15-bit display mode. A CPU read must see all prior GPU writes.
The backend now keeps a separate conservative rectangle for GPU writes that
have not reached the CPU array. It reads that rectangle into the full-width CPU
array with an explicit row stride. It then clears the CPU readback debt.

The rectangle includes clipped primitives, fill segments and copy destinations.
It also retains framebuffer clearing debt across depth24 transitions. GPU
uploads and queued draws still finish before packing and reading. Texture
sampling can consume packing debt without consuming CPU readback debt. A
conservative union is safe for GPU-to-CPU reads because the GPU image is current;
CPU-to-GPU uploads still use their exact rectangle list.

This does not change raster rules, read values, guest clocks, draw order or
precision. The original
interlaced row clipping remains in place. State recreation clears the new debt;
CPU-authority mode retains its existing no-readback behavior.

During 24-bit display, the CPU array owns packed RGB888 pixels. GP0 drawing
commands still operate on 15-bit halfwords, so their software raster writes
also update this array. Reads retain those bytes rather than replacing them
with the FBO, which can omit large RGB888 uploads. The latched depth24 mode
allows the entry readback to finish earlier 15-bit drawing before ownership
changes. Existing movie-band clearing and queued texture upload order on
return to 15-bit mode remain in place.
Before a 24-bit software mirror writes CPU pixels, the renderer flushes any
pending small CPU-backed upload. The upload queue holds rectangles, not pixel
snapshots. Without this order, the later GPU operation can consume already
blended or copied pixels and apply the same operation twice.

## Verification

`runtime/tests/test_gl_readback_region.c` uses an actual hidden SDL/OpenGL
context. It checks the complete native CPU image against full hardware readback
after ordered synthetic workloads. It covers single pixels, odd-width row
stride, disjoint uploads, texture dependencies, wrapping fills, overlapping
copies, masks, blending, clipping, precision margins and state restore. A depth24 exit followed by an immediate
CPU read verifies that cleared movie pixels are visible and newer overlapping
texture uploads survive. It tests this existing clear policy, not full movie
playback or depth24 raster accuracy. An additional original fixture checks a
black half blend and a VRAM copy during 24-bit display against explicit native
halfword values, including untouched packed bytes after a CPU transfer read.
It checks the entry readback and the existing clear on return to 15-bit mode.
An outside-band small-upload fixture also checks upload-before-half-blend and
upload-before-overlapping-copy across the return to 15-bit mode. It retains
explicit expected halfwords and prints the observed native GPU values.
The hardware rasterizer is shared by the comparison; this proves coherence,
not independent raster accuracy. No retail assets are required.

On 2026-10-09 an original Linux SDL2 offscreen/EGL unit completed on NVIDIA
GeForce RTX 3060 Laptop GPU, OpenGL 4.6.0 driver 615.71.09. The accepted source
`015fe7a` failed exactly the three new depth24 blend/copy/read assertions at
both native and 4x scale. The first candidate `147eb2d` passed all 176 checks at both scales.
Both variants completed the existing hardware tests and observed the expected
`0x7fff` native test pixel. The input manifest and complete unit/closure
receipts are attached to PS1G-34. This proves the synthetic ownership behavior,
not The X-Files menu/NewGame result, Windows SDL3 or a qualified replay.

The additional outside-band upload-order fixture reproduced two failures in
`147eb2d` at both scales: the GPU blend returned `0x1ce7` rather than `0x3def`,
and the overlapping copy returned `0x001f` rather than `0x03e0`. Flushing the
pending upload before the mirror write produced the expected native values
and passed all 185 checks at both scales on that same SDL2/NVIDIA platform.
The original movie-band, mask, clipping, precision and copy controls remain.
Complete R6 inputs and terminal/closure receipts are attached to PS1G-34;
these extra checks still do not qualify a retail title or SDL3 product.

Run `python runtime/tests/run_gl_readback_region.py --compiler-bin <mingw-bin>
--sdl-root <deps> --output <new-directory>`. The SDL dependency root must contain
`sdl3-src/include` and `sdl3-build/libSDL3.a`. The Windows runner uses hidden
windows. It tests
native and 4x scales. `--gl-source <old-gpu_gl_renderer.c> --expect-unbounded`
accepts an unbounded implementation only when pixel comparisons pass and
exactly the small-transfer assertion fails. An older implementation with a
coherence defect (including the depth24 clear) is correctly rejected too. Each command/result is saved in `receipt.json` under a fresh run directory inside the output root. Configure `runtime/` with `BUILD_TESTING=ON` and `PSX_GL_READBACK_SDL_ROOT=<deps>` on MinGW to register `gl_readback_region_test`; run it with `ctest --test-dir <build> -R gl_readback_region_test --output-on-failure`. Missing dependencies print a configure-time message and do not count as a hardware pass.

## Prior art and scope

Existing renderer coherence code supplied the ordering and dirty-area rules.
DuckStation's software readback path was consulted as an alternative. This correction keeps the frozen branch's GPU
authority and needs no title hook or controller policy change. Future work can
reduce row submissions or use a more precise dirty set if measured demand
justifies it.

For Clang/MinGW builds, set `PSX_GL_READBACK_COMPILER_BIN` to a GCC/MinGW
bin directory containing both gcc.exe and g++.exe. CMake checks these files
before registering the hardware test. Both the configure step and the runner
also validate the SDL3 include directory and static library. `gl_readback_runner_test` separately
checks strict result parsing without a GPU or compiler. A negative run passes
only for exactly one failure named single-pixel bounded transfer.
