#ifndef PSXRECOMP_SURFACE_MATERIALS_H
#define PSXRECOMP_SURFACE_MATERIALS_H

/* Surface materials — game-agnostic "what is this polygon made of" tagging.
 *
 * A textured primitive is identified by the Beetle-compatible texture key
 * (CRC of the original CPU->VRAM upload + CRC of the live CLUT, the same
 * `<texture>-<palette>` names an HD texture dump uses; see hd_texture_pack.h).
 * A per-game table (surface_materials.toml) maps keys to materials, and the
 * renderer applies the material's shader branch to every prim carrying that
 * key. Nothing here is per-game code: tagging a new game's water is a data
 * file, found with the surface_probe / surface_census debug commands.
 *
 * Enhancement tier only (ENHANCEMENTS.md, CLAUDE.md Rule -1 scope): with no
 * table loaded and the probe off, surface_materials_active() is 0, no
 * tracking runs, every prim gets material 0, and output is byte-identical.
 * See docs/SURFACE_MATERIALS.md. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURFACE_MAT_SLOTS     16  /* index 0 = none */
#define SURFACE_MAT_HIGHLIGHT 15  /* reserved: debug highlight */

enum SurfaceMaterialKind {
    SURFACE_KIND_NONE = 0,
    SURFACE_KIND_WATER = 1,
    SURFACE_KIND_HIGHLIGHT = 2
};

/* Parameters as the shader consumes them. Units: texels / seconds. */
typedef struct SurfaceMaterial {
    int   kind;             /* enum SurfaceMaterialKind */
    float ripple_amplitude; /* UV displacement, texels */
    float ripple_scale;     /* ripple spatial frequency, radians per texel */
    float ripple_speed;     /* radians per second */
    float flow_u, flow_v;   /* texture scroll, texels per second */
    float glint;            /* specular strength, 0..1 */
    float glint_power;      /* specular exponent */
    float tint[3];          /* multiplicative tint */
    float tint_mix;         /* 0 = untinted, 1 = fully tinted */
    char  name[48];
} SurfaceMaterial;

/* Fast gate for hot paths (tracking + classification). */
extern int g_surface_materials_active;
static inline int surface_materials_active(void) { return g_surface_materials_active; }

/* Startup: loads $PSXRECOMP_SURFACE_MATERIALS when set. Safe to call twice. */
void surface_materials_init_from_env(void);

/* Load / replace the table. Returns 1 on success; on failure the previous
 * table is kept and error is filled. load_text is the same parser on a
 * string (tests, TCP). */
int  surface_materials_load_file(const char *path, char *error, size_t cap);
int  surface_materials_load_text(const char *toml_text, char *error, size_t cap);
void surface_materials_clear(void);

/* Material slot for a key (0 = none). */
int  surface_materials_lookup(uint32_t texture_hash, uint32_t palette_hash);
const SurfaceMaterial *surface_material_get(int slot); /* NULL if out of range */
int  surface_materials_count(void);  /* materials defined (excl. highlight) */
int  surface_materials_rule_count(void);

/* Debug: paint every prim with this key in the highlight material.
 * any_palette ignores the palette half. on=0 clears. */
void surface_materials_set_highlight(uint32_t texture_hash, uint32_t palette_hash,
                                     int any_palette, int on);
/* Debug: record per-prim samples for surface_probe / surface_census even
 * with no table loaded. */
void surface_materials_set_probe(int on);

/* ---- VRAM residency feed (gpu_render.c facade + renderer) ---- */
void surface_note_upload(int x, int y, int w, int h, const uint16_t *words);
void surface_note_vram_write(int x, int y, int w, int h); /* fill/copy dest/draws */

/* Classify one textured prim. bbox = drawn VRAM rect (inclusive), used for
 * probing. lim = inclusive sampled uv bounds {u_min, v_min, u_max, v_max}.
 * Returns the material slot (0 = none). */
int  surface_classify_prim(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                           const int lim[4], const uint16_t *vram,
                           int x0, int y0, int x1, int y1);

/* Frame boundary (vblank): rotates the probe sample buffers, advances time. */
void  surface_frame_end(void);
float surface_time_seconds(void); /* guest-frame time (pauses with the game) */

/* TCP bodies (no surrounding braces/id). Return bytes written. */
int surface_probe_json(int vram_x, int vram_y, char *buf, int cap);
int surface_census_json(int limit, char *buf, int cap);
int surface_status_json(char *buf, int cap);

/* Parse "1a2b3c4d-5e6f7a8b" / "1a2b3c4d-*". Returns 1 on success. */
int surface_parse_key(const char *text, uint32_t *texture_hash,
                      uint32_t *palette_hash, int *any_palette);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_SURFACE_MATERIALS_H */
