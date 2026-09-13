# Private Nightmare Creatures II FMV test

This candidate adds a presentation policy to Wave 4 c2 (`4f534f2cf04df6fb677f108e5475dcec1e7573d3`). It blacks out physical VRAM rows that have no completed CPU upload during recent MDEC activity. It does not change guest VRAM, raw debug screenshots, display placement, DMA timing, or the earlier texture correction.

The policy derives from TechnicallyComputers/Claude's Colony Wars work in upstream commits [1f02ca1](https://github.com/mstan/psxrecomp/commit/1f02ca1855b6f351eaef31f5bfe222860f41f74f) and [5c77c40](https://github.com/mstan/psxrecomp/commit/5c77c40550b66153fcfb9ede75b51d9bd4d332fc). This version checks the activity edge before recording an upload and resets both activity history and coverage after a save-state load. It is a display enhancement, not evidence of PS1 hardware behavior.

The source-owned fixture checks unwritten rows, committed RGB data, physical-row wrap, inactive still images, depth transitions, the first upload before presentation, and deterministic reset after save-state load. Compile on the qualified Windows MinGW GCC 16.1 toolchain with `-O2 -UNDEBUG -flto -fwhole-program -ffunction-sections -fdata-sections -Wl,--gc-sections -I runtime/include runtime/tests/test_depth24_row_policy.c`. Other toolchains are unqualified.

Coverage is deliberately not serialized. Restoring during a movie can show black until the next upload; a movie uploaded before the depth transition can also wait for the next upload. Automated baseline samples from software and OpenGL did not reproduce the reported strips. Alex subsequently confirmed the FMV Border Test works and explicitly requested promotion on September 13, 2026. This qualifies the reported Nightmare Creatures II route through operator acceptance; it does not qualify other titles or prove hardware fidelity. Keep this as a title-specific source override of c2 when building the paired Windows and Linux packages.

Canonical investigation: https://app.notion.com/p/3d91cf959a4c81bb9ce4c483d897a1cc
