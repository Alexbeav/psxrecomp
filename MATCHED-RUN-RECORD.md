# Matched run record: control vs fresh-generation candidate (2026-09-22)

## Executables

| | Control (black screen) | Fresh-generation candidate |
| --- | --- | --- |
| path | `_local\integration-kit\bld\Syphon_Filter_2_Recompiled.exe` | `_local\fresh-kit\bld\Syphon_Filter_2_Recompiled.exe` |
| SHA-256 | `97282fb89d1a0420572baad527dc16f71539fee9a69d323ef2415ab197901bff` | `df7607578064b630fc2062f51cbeccc052077393ab1118b49b87d19ddab66ab1` |
| size | 47,596,984 B | 66,206,435 B |
| generated BIOS | 1,660,756 B dispatch / 26,757,658 B full | 7,384,719 B dispatch / 43,025,623 B full |
| generated game files | 113 (from old emitters) | 99 (from merged emitters) |
| emitters that produced its code | `f2798b9f…` / `b39b485d…` | `72c5fc25…` / `08aff125…` |
| input focus fix | **absent** | **absent** (fixes isolated on branch `input-fixes` @ `87ca937c`) |
| framework | merge `f3de7154` | merge `f3de7154` |

The single variable is **which generators produced the linked generated code**.

## Effective settings (identical, paths adapted per kit)

```toml
[video]
renderer = "opengl"
supersampling = 4
window_width = 1920
antialiasing = true
texture_filtering = "nearest"
fullscreen = 0
aspect_ratio = "4:3"
frame_interpolation = false
fast_boot = true

[launcher]
skip_launcher = true

[bios]
path = "<projects>/PSX-Ports/suikoden-ii/psxrecomp/bios/SCPH1001.BIN"
```

Disc and memcard differ only in kit-root path:

| | Control | Fresh |
| --- | --- | --- |
| disc | `_local/integration-kit/disc/Syphon Filter 2 (USA) (Disc 1).cue` | `_local/fresh-kit/disc/Syphon Filter 2 (USA) (Disc 1).cue` |
| memcard dir | `_local/integration-kit/saves` | `_local/fresh-kit/saves` |

**Note:** `settings.toml` must sit **beside the executable** (`exe_dir_from_argv(argv[0])`,
`main.cpp:13891`), not in the kit root. The fresh kit initially had none, so it would
have run with defaults; a matching file was written to `fresh-kit/bld/settings.toml`
before the matched run.

## Inputs

| Input | Value |
| --- | --- |
| BIOS dump | `<projects>\PSX-Ports\suikoden-ii\psxrecomp\bios\SCPH1001.BIN`, sha256 `71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3` |
| disc | Syphon Filter 2 (USA) (Disc 1).cue, serial SCUS-94451, region NTSC-U |
| boot exe | SCUS_944.51, sha256 `75a360bf7465dfdec85c14f9ba93862aae2531b48d83fd8d82ba8c9fffa13d33` |
| game.toml | sha256 `813d40b794f68ca85c63416a81264f6102fe3fa16d4a3e0805e8b64ad8ac3355` |

## Launch arguments (identical)

```
--game <kit>/game.toml --disc <kit>/disc/Syphon Filter 2 (USA) (Disc 1).cue
```
with `PSX_NO_LAUNCHER=1`, `fast_boot = true` from settings, HLE shell-skip engaged
(`bios_boot=HLE (shell skipped)`).

For introspective runs: `PSX_DEBUG_TOOLS=1`, `PSX_DEBUG_PORT=437x`,
`PSX_GL_PRESENT_PROBE=1`. Note the plain `bld` builds have `PSX_DEBUG_TOOLS=OFF`,
so a debug port is ignored there; instrumentation needs the `-DPSX_DEBUG_TOOLS=ON`
build (`bld-dbg`).

## Observables to capture (identical for both runs)

1. **Visible output** — `screenshot_file` PNG, analysed for distinct colour count and
   non-black sample count.
2. **Guest execution** — `get_registers` (PC, `$ra`, registers) sampled repeatedly.
3. **Display state** — `gpu_state` (display_x/y, h_display, v_display, draw_area,
   gp0_* counters).
4. **VRAM content** — `vram_peek` sweep across the display row band and at x=768.
5. **Present ring** — `gl_present_ring` (path, display rect, backbuffer sample `px`,
   source-FBO sample `src_valid`).

## Outcome rules (corrected before interpreting)

- **Fresh build renders:** regeneration changed the outcome. It does **not** prove the
  entire merge is sound.
- **Fresh build stays black:** regeneration alone did not resolve it. That neither
  excludes a generation problem nor proves an x=768 framebuffer defect.
- **Compilation succeeds:** the generated code is accepted by the compiler and linker.
  Runtime correctness remains untested.

Judgement order: visible output and guest execution first; then establish whether
x=768 actually contains a framebuffer (image data there does not prove it is the
intended display source — it could be texture memory); then features; then the recipe.

## First result (uninstrumented `bld`, 60 s run)

Fresh candidate, same settings and arguments:

```
psxrecomp: bios_backend=LLE (recompiled BIOS)  bios_boot=HLE (shell skipped)  image=SCPH-1001
psxrecomp runtime: executing from PC=0xBFC00000
psxrecomp: thread scheduler = HLE (deterministic TCB) (PSX_HLE_SCHEDULER)
```

Then no further output, process alive at 22.8 s CPU / 13 threads / responding. **Same
observable symptom as the control: boots, runs, stops logging at the reset vector,
stays alive and busy.** No debug port because `PSX_DEBUG_TOOLS=OFF` in that build, so
the pixel/VRAM/register capture requires the instrumented build.
