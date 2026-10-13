# Post effects

Elective effects on the composed game image (OpenGL). All of them are off by
default. With none active, nothing below runs and the frame is the stock one.

Every effect is a **chain** of full-screen fragment passes run by one
executor (`runtime/src/gl_postfx.inc`). Three stages run in order, each from
a fresh copy of the image:

| stage | switch | chain |
|---|---|---|
| bloom | `[video] bloom = 0..2` (`PSX_BLOOM`) | built-in `bloom` |
| user | `[video] post_shader = "<file>.pfx"` (`PSX_POST_SHADER`) | your preset |
| AA | `[video] antialiasing_mode = "fxaa" \| "fxaa_hq"` | built-in `fxaa` / `fxaa_hq` |

They run on whichever thread presents (the render thread included), before
hold-last capture, screenshots and the OSD. Smooth motion's generated frames
run bloom and the user chain; AA stays on real frames, as before. The executor
owns targets, programs, uniforms and GL state, and puts back everything it
touches.

## Preset format (`.pfx`)

```
pfx 1                              # required first line
pass <name>                        # the last pass writes the window
  shader <file.glsl|builtin:name>  # file paths resolve next to the preset
  input <sampler> <source|mask|previous|<earlier pass>>
  scale <f>                        # target size as a fraction of the game rect (default 1)
  format rgba8|rgba16f             # default rgba8
  filter linear|nearest            # how later passes sample this target
  uniform <name> <f> [f [f [f]]]   # float .. vec4
```

A shader file is a GLSL 330 fragment body. It is appended to this header:

```glsl
in vec2 v_uv; out vec4 o;      // v_uv: 0..1 over the game rect, bottom-up
uniform vec2 u_rcp, u_size;    // this pass's target: 1/size and size in px
uniform ivec2 u_org;           // window origin of the game rect
uniform sampler2D <sampler>;   // one per input
```

Built-in shaders: `blur` (9-tap Gaussian along `u_dir`, in px), `fxaa`,
`bloom_bright` and `bloom_composite`. The built-in presets are listed in
`runtime/src/postfx_chain.c`. A preset that fails to parse, or a pass that
fails to compile, is logged and its stage skipped.

Example: invert the image.

```
pfx 1
pass invert
  shader invert.glsl
  input u_tex source
```

with `invert.glsl` containing:

```glsl
void main(){ o = vec4(1.0 - texelFetch(u_tex, ivec2(gl_FragCoord.xy) - u_org, 0).rgb, 1.0); }
```

## The mask input

`mask` is 1 where the last thing drawn was a 3D primitive (proven from GTE
data, so it needs PGXP geometry) or an additive semi-transparent primitive
(blend modes 1 and 3). It is 0 where an opaque 2D primitive drew last, such as
a sprite, a fill or the HUD. Only while an active chain reads `mask` does every
draw keep that mark in stencil bit 1 (bit 0 is the PS1 mask-bit mirror). The
mark follows the picture through native-wide centre copies and Smooth motion
surfaces. At the present it becomes a window-sized R8 texture. A chain that
does not read `mask` costs none of this.

Bloom uses the mask at both ends: only lit pixels feed its bright pass, and
only lit pixels take the glow back. HUD pixels are therefore never changed,
even next to a lamp.

## Tests

- `postfx_chain_test`: the format, built-in presets, errors and file import.
- `gl_bloom_test`: bloom on 2D, additive and 3D primitives, plus a HUD beside
  and over a lamp. Also an imported chain (byte-exact invert), the mask input
  (the HUD unlit, the 3D scene lit), a bad preset, a pass that does not
  compile, and GL state restored with every stage on. It runs on the caller
  thread and on the render thread.
- `gl_post_aa_test`: FXAA on the pipeline. Its output is byte-identical to
  the former fixed pass.
