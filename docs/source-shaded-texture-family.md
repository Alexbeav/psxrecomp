# Source shaded textured polygon variants

Pepsiman's successive 39 and 31 scope stops expose incomplete flag-family
admission. Alongside the confirmed untextured triangle correction, this
fixture qualifies the remaining shaded textured polygon variants 35/36/37
and 3D/3F, retaining already admitted 34/3C/3E as source controls.

The existing renderer and work projector own raw texture, modulation,
semitransparency, Gouraud interpolation, masking, cache and split-quad work.
The production change only admits the qualified opcodes. It does not change
packet lengths, rendering, timing, movie input, geometry guards or reset
policy. These variants were not each independently encountered in the title;
their admission is supported by the complete authored source matrix.

The CPU/MMIO driver seeds mixed transparent, mask-bit and ordinary texture
and destination pixels, then draws two identical polygons without clearing
the texture cache or framebuffer. It covers eight opcodes, three texture
depths, four blend modes, four mask modes, dithering off/on and disjoint or
overlapping texture targets. The source owns all CPU loads/stores and GPU
dispatches. Declared waits restore draw credit between dispatches.

All 1536 complete final VRAM images match stock Octoshock 2.3. The passive
observer preserves each image and authored program identity, and records
4608 command/cache work values across both repeated triangles or split
quads. O0/O2 native fixtures match every image and work value. Before the
admission change, the native fixture stops at 35 after 192 opcode 34 controls.

The golden manifest binds source lineage a15b31a, stock core, passive observer
and authored driver. No retail data or emulator states are included. Run
CTest -R tas_gpu_shaded_texture_family. This is source-profile qualification;
full unchanged Pepsiman and Tekken replays and the Pepsiman ending witness
remain separate acceptance gates. No whole-game or hardware claim follows.
