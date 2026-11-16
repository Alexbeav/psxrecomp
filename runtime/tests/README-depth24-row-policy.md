# Depth24 movie row policy

This is a framework presentation policy. It applies to every title that plays 24-bit MDEC movies.

While MDEC is streaming, `gpu_depth24_present_row` presents a physical VRAM row as black when no completed CPU-to-VRAM upload has landed on it since the stream began. Letterbox bars then show black instead of leftover 15-bit screens. Without the policy, a double-buffered movie shows those leftovers at different rows in each band, so the bars flicker.

The policy changes presentation only. It does not change guest VRAM, raw debug screenshots, display placement, DMA timing, or GPU state. It is a display policy, not a claim about PS1 scanout hardware. TAS replays compare VRAM and CPU state, so they cannot see it. Visual checks are the only coverage.

Rules:

- A completed upload marks its rows while the display is 24-bit and MDEC is streaming. Rectangles wrap at physical row 512.
- The first streaming observation clears coverage, so rows left by 24-bit still images before the movie become bars.
- When MDEC is not streaming, rows are not masked. Static 24-bit images stay visible.
- A display-depth change clears coverage.
- A GPU reset and a save-state load reset coverage and streaming history. Coverage is not serialized.

Known limits:

- After a save-state restore during a movie, the screen shows black until the next upload.
- A movie frame uploaded before the depth change waits for the next upload before it shows.

The policy derives from TechnicallyComputers/Claude's Colony Wars work in upstream commits [1f02ca1](https://github.com/mstan/psxrecomp/commit/1f02ca1855b6f351eaef31f5bfe222860f41f74f) and [5c77c40](https://github.com/mstan/psxrecomp/commit/5c77c40550b66153fcfb9ede75b51d9bd4d332fc). This version checks the streaming edge before it records an upload, and resets coverage after a save-state load. It shipped as a title override for Colony Wars (Wave 3) and Nightmare Creatures II (Wave 4, accepted by Alex on 2026-09-13). It became framework-wide on the Wave 5 pin line under PS1B-196.

`test_depth24_row_policy.c` includes `gpu.c` and drives the real upload producer, display-mode transition, and row presenter with synthetic pixels. It needs no game or BIOS data. CTest registers it as `depth24_row_policy_test` on GCC only, because it relies on whole-program LTO with `--gc-sections` to drop unused GPU services. Its flags are `-O2 -UNDEBUG -flto -fwhole-program -ffunction-sections -fdata-sections -Wl,--gc-sections`.

Visual retest on each new pin: the FMV bars in Colony Wars, Nightmare Creatures II, Resident Evil Director's Cut, Resident Evil 2 and Metal Gear Solid.
