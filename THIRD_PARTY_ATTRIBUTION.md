# Third-Party Attribution

## OpenBIOS — PCSX-Redux's free PS1 BIOS

[OpenBIOS](https://github.com/grumpycoders/pcsx-redux) (src/mips/openbios) by
the PCSX-Redux authors, licensed **MIT** (notice: `bios/OpenBIOS.LICENSE`;
the binary also links permissively-licensed code from
[uC-sdk](https://github.com/grumpycoders/uC-sdk), noted there too). Vendored
as the prebuilt image `bios/openbios.bin` (pin and build recipe recorded in
`bios/OpenBIOS.toml`) and statically recompiled by
`psxrecomp-bios --config bios/OpenBIOS.toml` exactly like the retail BIOS.
Normal runtime builds stage it automatically so players supply only a disc;
`bios/OpenBIOS.LICENSE` always rides alongside the shipped image.


## TinyCC (TCC) — toolchain-free overlay compiler shipped to players

[TinyCC](https://bellard.org/tcc/) by Fabrice Bellard and contributors, licensed
**LGPL-2.1**. Not vendored in this repository — no TinyCC source or binary is
tracked here. It is invoked as a **separate subprocess** by
`tools/compile_overlays.py` (`--compiler tcc`, `--tcc &lt;binary&gt;`) to build overlay
shards into a DLL, so players need no compiler of their own. Nothing in the
runtime links against libtcc, so this is aggregation with a separate program
rather than LGPL linkage.

The runtime expects an end-user bundle at
`&lt;exe_dir&gt;/overlay_toolchain/{python/, tcc/tcc.exe, compile_overlays.py, …}`
(`runtime/src/main.cpp`). **No script in this repository populates that
directory**, so if release packaging supplies it, that step lives outside this
repo and the TinyCC license notice must be shipped alongside it there.

Developers with `gcc` on `PATH` use the gcc tier instead; the bundled tcc matters
only for end-user release packages (`docs/BUILDING.md`).

## JRickey / gba-recomp — verified-enhancement shadow + screen color science

The verified-enhancement QoL layer (`feat/shadow-enhancements`) reuses two
engine-agnostic pieces originally authored by Jrickey in
[JRickey/gba-recomp](https://github.com/JRickey/gba-recomp), licensed
**MIT OR Apache-2.0**, used with permission:

- **`ShadowVerifier`** — the envelope-correlation differential self-check,
  probation auto-gain calibration, and prove/strike/pause state machine.
  Original: `crates/gba-core/src/shadow.rs`.
  This repo: `runtime/src/audio_shadow.c`, `runtime/include/audio_shadow.h`
  (C re-implementation, via the gbarecomp C++ port `src/gba/audio_shadow.*`
  and the snesrecomp C port `runner/src/snes/audio_shadow.*`; the algorithm is
  unchanged).

- **Color-science core** (xyY→XYZ, primaries→matrix, Bradford chromatic
  adaptation, sRGB OETF) used to bake the present-time screen-color LUT.
  Original: `crates/screen/src/{color,profile,lut}.rs`.
  This repo: `runtime/src/color_lut.c`, `runtime/include/color_lut.h`
  (C re-implementation, via the gbarecomp C++ port `src/runtime/color_lut.*`).

### PSX-specific work (ours)

- The **CRT / composite / Trinitron** display panel models in `color_lut.c`
  (the GBA port modelled a handheld LCD; a console scanned out to a TV needs a
  CRT/composite model instead) — SMPTE-C / Trinitron-class phosphor gamuts,
  CRT gamma, black-lift.
- The **SPU float shadow render** (`runtime/src/spu_shadow.c`,
  `runtime/include/spu_shadow.h`): 4-point cubic resampling + float headroom
  re-render of the PS1 SPU ADPCM voice mix, driven from a read-only tap on the
  canon `spu.c` voice state. This is console-specific (the SNES analog re-renders
  the S-DSP; the GBA analog re-renders the MP2K software mixer).
- The tap plumbing in `runtime/src/spu.c` and `runtime/include/spu.h`.

All reuse keeps the original copyright and dual MIT/Apache-2.0 license.
