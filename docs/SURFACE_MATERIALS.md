# Surface materials — tag what a polygon is made of, shade it like a modern game

Enhancement tier (CLAUDE.md Rule -1 scope, `ENHANCEMENTS.md`). Off by default;
with no table loaded and the probe off, nothing is tracked and every primitive
gets material 0, so the shader's material branch never runs.
`tools/surface_material_smoke.py` checks that clearing the feature restores
the first render pixel for pixel. Tracking runs only on the OpenGL backend
(the only consumer), and a savestate load drops residency, because restored
VRAM is not an upload.

## Why this exists

The goal is techniques that make *any* PS1 game look better, not one-off work
for one title. Most modern effects (water, lava, glass, metal, emissive signs)
need the renderer to know what a surface *is*. A PS1 GPU only ever sees
"textured triangle, page X, CLUT Y". Surface materials add that knowledge
as **data**: a per-game `surface_materials.toml` maps texture keys to
materials, and the renderer applies the material's shader to every primitive
drawn with that texture. Adding water to a new game is a data file found with
three debug commands. It needs no code changes.

## Pipeline

1. **Texture identity.** Every CPU→VRAM upload (`GP0(A0h)`) is recorded with
   the CRC of its words by the residency tracker in `hd_texture_pack.cpp`.
   Fills, VRAM→VRAM copies, single-pixel writes and every primitive's drawn
   rect cut the residencies they overwrite, so render targets lose identity.
   A textured primitive's key is `<upload CRC>-<CLUT CRC>`, the same
   Beetle-compatible key HD texture dumps use. Material tags and HD
   replacement packs therefore share one naming scheme and one tracker.
2. **Classification.** `surface_classify_prim` (called by the renderer per
   textured triangle) asks the tracker which upload still holds every texel
   the primitive samples (its exact uv bounds), hashes the live CLUT and looks
   the key up. A result is cached per (texpage, uv rect) until residency
   changes (`hd_texture_pack_tracking_generation`). The palette half is always
   recomputed, so palette-cycled textures stay correct.
3. **Rendering.** The OpenGL backend writes the material slot into a per-vertex
   attribute (`TEX_VS` location 10). The `TEX_FS` material branch reads the
   slot's parameters from uniform arrays. Slot 0 skips the branch entirely.
   The software and Vulkan backends ignore materials for now and draw the
   faithful image.

## Table format

`PSXRECOMP_SURFACE_MATERIALS=/path/to/surface_materials.toml` loads at startup.
The `surface_load` TCP command reloads it live while tuning.

```toml
format_version = 1

[materials.dock_water]
kind = "water"
ripple_amplitude = 1.25   # uv displacement, texels
ripple_scale     = 0.12   # wave frequency, radians per texel
ripple_speed     = 1.6    # radians per second (guest time: pauses with the game)
flow             = [0, 0] # extra texture scroll, texels per second
glint            = 0.35   # sun-glint strength, 0..1 (scaled by the prim's own lighting)
glint_power      = 24     # glint sharpness
tint             = [0.85, 0.95, 1.0]
tint_mix         = 0.2    # 0 = untinted

[[surface]]
key      = "1a2b3c4d-*"   # tex-pal; "*" = any palette (needed for palette-cycled water)
material = "dock_water"
note     = "Dock: main water plane"
```

Up to 14 materials per table. Slot 15 is reserved for the debug highlight.
Unknown kinds, unknown material names, duplicate keys and malformed keys are
load errors, and a failed load keeps the previous table.

### Water (v1)

Three crossing wave trains are evaluated in **texel space**, so ripples are
anchored to the surface and move with the camera like real water. The
analytic height gradient displaces the sampled uv, which gives a refraction
wobble, and tilts a normal for a sun glint. The glint is scaled by the
primitive's own vertex lighting, so it dies in areas the game already
darkened. The displaced uv wraps inside the primitive's own sampled rect, so
`flow` scrolls the texture instead of smearing its edge. Semi-transparent
water keeps PS1 blending: the material changes the colour before the blend.

What v1 deliberately does not do yet: **reflections**. A planar or
screen-space reflection needs per-pixel depth and the camera. Both come from
the shared depth layer that the Alien Resurrection frame-rate work owns (a
depth attachment on the GL hi-res FBO filled from PGXP's projected SZ). Once
it lands, water gets SSR and a fresnel term from the real view angle, and
the same depth feeds shadows and GI.

## Tagging a game's water (the workflow)

Run with the OpenGL renderer and debug tools (`PSX_DEBUG_TOOLS=ON`). Commands
are documented in `TCP_COMMANDS.md`.

1. Start with the probe armed before the scene loads, so its textures are
   tracked: either set `PSXRECOMP_SURFACE_MATERIALS` to any table, or send
   `{"cmd":"surface_probe_on","on":1}` and then re-enter the area. A texture
   uploaded before tracking started reports `"status":"untracked"`.
2. Stand where the water fills the screen and run
   `{"cmd":"surface_census","limit":20}`. Large-area keys come first, and
   the `bbox` shows where each one is drawn in VRAM.
3. Point at the water with `{"cmd":"surface_probe","x":X,"y":Y}`, where X/Y
   are VRAM coordinates (`gpu_state` `display_x/display_y` + screen pixel).
   It lists every textured primitive under that pixel, topmost first, with
   its key.
4. Confirm the tag with `{"cmd":"surface_highlight","key":"<tex>-*"}`. Every
   primitive with that key pulses magenta. Clear it with
   `{"cmd":"surface_highlight"}`.
5. Add a `[[surface]]` rule, `surface_load` the file, and tune parameters
   live.

If the water's palette animates (a different `pal` every few frames with the
same `tex`), use the `tex-*` wildcard. If the game rewrites the water texture
itself each frame (a new `tex` each frame), the key will not be stable.
Note that in the census and file a framework issue; that case needs a
signature-based matcher (planned, see Roadmap).

### Metal Gear Solid, Dock (first target)

Tag the dock water this way, ship the table with the MGS port, and judge it
on a real playthrough. The decompiled MGS source (FoxdieTeam/mgs_reversing)
helps cross-check what the Dock draws, but the tag itself is just texture
keys. Nothing MGS-specific goes into the runtime.

## Limits (current)

- OpenGL backend only. Software and Vulkan draw the faithful image.
- Tracker residency is not saved in savestates yet. Loading a state drops it,
  and textures regain identity when the game re-uploads them (any area change).
  `hd_texture_pack_tracking_state_save/load` already exist for this.
- The probe keeps only the last frame that drew textured primitives.

## Roadmap

1. Reflections and fresnel for water, after the shared depth layer lands.
2. Dynamic shadows and GI. These also need the depth layer and the game's
   light and camera state. MGS's decomp names those structures. Off-screen
   culling limits shadow casters (the game never submits what it culled).
3. Signature matching for textures that are re-uploaded with changing
   content (keyed by VRAM page/rect + size instead of CRC).
4. More kinds: `lava` (emissive + heat shimmer), `glass`, `metal` (env
   reflection), `emissive` (bloom source).
5. Vulkan backend parity.

## Verification

- `surface_materials_test` (ctest): key parsing, table errors, the
  default-off gate, residency identity, render-to-texture invalidation, exact
  vs wildcard palettes, highlight, probe/census output.
- `tools/surface_material_smoke.py`: drives the real GL renderer through
  `gp0_inject` (upload, textured quad), discovers the key with the census,
  applies water, checks that it changes and animates, checks the highlight,
  then checks that output with the feature cleared is pixel-identical to the
  first render.
