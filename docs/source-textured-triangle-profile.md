# Source textured triangle flag variants

GP0 25/26/27 join the existing24 path in the optional source command profile.
The existing generic software renderer already interprets raw modulation and
semi-transparency bits. Command admission now reaches that path for all four
flat-textured triangle variants. Seven-word packet, one-word readiness,
84+180 setup work, doubled pixel-span cost, tpage latch and retained texture
cache feedback are unchanged and covered by the production-header fixture.

CLAIM / DERIVED-FROM: original Octoshock2.3 `gpu.cpp` ProcessFIFO,
`gpu_polygon.cpp` Commands_20_3F and DrawSpan at upstream
`a15b31a46bdac27d843d3ebbc5a860012d8452fb`. This is source compatibility.

ORACLE: 384 authored cases exercise opcodes24–27, all three texture depths,
four blend modes, four destination mask combinations, raw/modulated color,
transparent/opaque/semitransparent texels and retained cache. Each complete
native1MiB VRAM image hash matches unmodified stock Octoshock2.3, bound in
`runtime/tests/source_gpu_textured_triangle_fixtures.json`. The external stock
fixture runs ordinary GPU MMIO from an authored ROM and verifies its wait-loop
marker. It imports no native state, BIOS or retail content. Source control is
the stock DLL; no patched core is used. Full oracle driver is in the campaign
checkpoint. This pixel comparison makes no new timing/hardware claim.

REPRODUCE: configure recompiler CMake tests and run CTest with pattern
`tas_gpu_textured_triangle`. O0/O2 compare every golden hash, packet dispatch,
work budget and texture-cache feedback. The before-correction fixture rejects
opcode25 at admission; the previous full Pepsiman replay stops at opcode26.
Full original movie, native ending and the unchanged Tekken regression remain
separate gates. Other command and state guards remain in force.
