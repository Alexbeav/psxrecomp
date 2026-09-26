/* internal_resolution.h — the "Internal resolution" setting: presets, their
 * persisted ids, and the integer scale each one asks the renderer for.
 *
 * Pure (no SDL, no GL) so both the config loader and a test can use it.
 *
 * VALUE ENCODING (shared with recomp-ui's Settings.internal_resolution):
 *   0   unset: the legacy [video] supersampling factor stands
 *   1   native (1x)
 *  -1   match display: the target is the monitor's pixel height
 *   N   (N >= 2) a target output height in lines (720, 1080, ... or a
 *       legacy "Nx" entry expressed as reference_lines * N)
 *
 * SCALE. The renderer only does integer scales (the pack of the hr surface
 * back to native VRAM is exact only then), so a target height becomes
 *     S = ceil(target_lines / reference_lines)
 * clamped to [1, s_max]. reference_lines is the title's usual display height
 * ([video] resolution_reference_lines, default 240): R4's races are 320x240,
 * so 1080p is S = 5 (1200 lines, area-resolved to 1080) and 4K is S = 9.
 * A 480-line mode (interlaced menus) renders at twice the target and is
 * resolved down. The backend then clamps S again to what it can allocate. */
#pragma once

#include <stddef.h>

#define PSX_IR_UNSET     0
#define PSX_IR_NATIVE    1
#define PSX_IR_DISPLAY  (-1)
#define PSX_IR_DEFAULT_REF_LINES 240
/* Custom line counts accepted from config (a 16K panel is 8640 lines). */
#define PSX_IR_MIN_LINES 2
#define PSX_IR_MAX_LINES 8640

typedef struct PsxIrPreset {
    const char *id;     /* stable id persisted in settings.toml */
    const char *label;  /* launcher label */
    int         value;  /* encoding above */
} PsxIrPreset;

static const PsxIrPreset k_psx_ir_presets[] = {
    { "native",  "Native",        PSX_IR_NATIVE  },
    { "720p",    "720p",          720            },
    { "1080p",   "1080p",         1080           },
    { "1440p",   "1440p",         1440           },
    { "4k",      "4K",            2160           },
    { "5k",      "5K",            2880           },
    { "8k",      "8K",            4320           },
    { "display", "Match display", PSX_IR_DISPLAY },
};
#define PSX_IR_PRESET_COUNT ((int)(sizeof(k_psx_ir_presets) / sizeof(k_psx_ir_presets[0])))

static inline int psx_ir_lower_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        if (ca != *b) return 0;
    }
    return *a == 0 && *b == 0;
}

/* 1 if value is a legal encoding (unset excluded). */
static inline int psx_ir_value_valid(int value) {
    return value == PSX_IR_NATIVE || value == PSX_IR_DISPLAY ||
           (value >= PSX_IR_MIN_LINES && value <= PSX_IR_MAX_LINES);
}

/* Parse a persisted value: a preset id (case-insensitive) or a decimal line
 * count in [PSX_IR_MIN_LINES, PSX_IR_MAX_LINES]. Returns 1 and stores the
 * value on success; 0 for anything else (the caller keeps its default). */
static inline int psx_ir_parse(const char *s, int *out) {
    if (!s || !*s) return 0;
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++)
        if (psx_ir_lower_eq(s, k_psx_ir_presets[i].id)) { *out = k_psx_ir_presets[i].value; return 1; }
    long v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; if (v > PSX_IR_MAX_LINES) return 0; }
    if (p == s || *p) return 0;
    if (v < PSX_IR_MIN_LINES) return 0;   /* a number is always lines, never "1 = native" */
    *out = (int)v;
    return 1;
}

/* The preset id for a value, or NULL for a custom line count (persist those
 * as an integer). */
static inline const char *psx_ir_id_for(int value) {
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++)
        if (k_psx_ir_presets[i].value == value) return k_psx_ir_presets[i].id;
    return NULL;
}

static inline const char *psx_ir_label_for(int value) {
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++)
        if (k_psx_ir_presets[i].value == value) return k_psx_ir_presets[i].label;
    return NULL;
}

/* Unclamped scale a value asks for. display_px_h <= 0 (unknown) makes Match
 * display 1 until the caller can measure the monitor. */
static inline int psx_ir_scale_for(int value, int ref_lines, int display_px_h) {
    if (ref_lines < 1) ref_lines = PSX_IR_DEFAULT_REF_LINES;
    int target;
    if (value == PSX_IR_DISPLAY) target = display_px_h;
    else if (value >= PSX_IR_MIN_LINES) target = value;
    else return 1;                                  /* native / unset */
    if (target <= ref_lines) return 1;
    return (target + ref_lines - 1) / ref_lines;
}

/* The scale to request: psx_ir_scale_for clamped to [1, s_max]. */
static inline int psx_resolve_internal_scale(int value, int ref_lines,
                                             int display_px_h, int s_max) {
    int s = psx_ir_scale_for(value, ref_lines, display_px_h);
    if (s_max >= 1 && s > s_max) s = s_max;
    return s < 1 ? 1 : s;
}

/* A legacy [video] supersampling factor as a value the launcher can show:
 * the preset that resolves to the same scale (1 -> Native, 3 -> 720p,
 * 5 -> 1080p, 6 -> 1440p, 9 -> 4K, 12 -> 5K, 18 -> 8K at 240 lines),
 * otherwise ref_lines * n as a custom entry ("2x (480 lines)"). */
static inline int psx_ir_from_supersampling(int n, int ref_lines) {
    if (ref_lines < 1) ref_lines = PSX_IR_DEFAULT_REF_LINES;
    if (n <= 1) return PSX_IR_NATIVE;
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++) {
        int v = k_psx_ir_presets[i].value;
        if (v >= PSX_IR_MIN_LINES && psx_ir_scale_for(v, ref_lines, 0) == n) return v;
    }
    long lines = (long)ref_lines * n;
    return lines > PSX_IR_MAX_LINES ? PSX_IR_MAX_LINES : (int)lines;
}
