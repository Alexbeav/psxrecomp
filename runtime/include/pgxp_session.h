#ifndef PSXRECOMP_PGXP_SESSION_H
#define PSXRECOMP_PGXP_SESSION_H

/*
 * Which PGXP corrections a session arms. main.cpp's renderer setup calls this
 * once per session (the first boot and every rematch), after mod activation;
 * it lives here so runtime/tests/test_pgxp_session.c drives the same code.
 *
 * Inputs, in order of precedence:
 *   1. env_*: the validation overrides PSX_GEOMETRY_CORRECTION,
 *      PSX_PERSPECTIVE_TEXTURING and PSX_PGXP_CPU_MODE (-1 when unset, else
 *      0 or 1). They win over everything, so an A/B run can switch PGXP off
 *      under an enabled mod.
 *   2. video_* OR mod_*: the [video] baseline (game.toml, then settings.toml
 *      and the launcher) and the psx.enhancement.pgxp request recorded by
 *      this session's activation (pgxp_mod_requested()). Either one arms a
 *      correction. A session whose plan is empty -- netplay clears it, or the
 *      player disabled the mod -- has no request and gets the baseline alone.
 *
 * The mod arms both geometry and texture correction; its CPU-mode option adds
 * tier-2 propagation.
 */

typedef struct PSXPgxpSessionInputs {
    int video_geometry;
    int video_texture;
    int video_cpu_mode;
    int env_geometry;
    int env_texture;
    int env_cpu_mode;
    int mod_enabled;
    int mod_cpu_mode;
} PSXPgxpSessionInputs;

typedef struct PSXPgxpSessionArm {
    int geometry;
    int texture;
    int cpu_mode;
} PSXPgxpSessionArm;

static inline int psx_pgxp_session_pick(int env, int baseline, int mod) {
    if (env >= 0) return env ? 1 : 0;
    return (baseline || mod) ? 1 : 0;
}

static inline PSXPgxpSessionArm psx_pgxp_session_resolve(
        const PSXPgxpSessionInputs* in) {
    PSXPgxpSessionArm arm;
    arm.geometry = psx_pgxp_session_pick(in->env_geometry, in->video_geometry,
                                         in->mod_enabled);
    arm.texture = psx_pgxp_session_pick(in->env_texture, in->video_texture,
                                        in->mod_enabled);
    arm.cpu_mode = psx_pgxp_session_pick(in->env_cpu_mode, in->video_cpu_mode,
                                         in->mod_enabled && in->mod_cpu_mode);
    return arm;
}

/* Parse one of the env overrides: unset -> -1, "0" or empty -> 0, else 1
 * (the rule the runtime has always used for them). */
static inline int psx_pgxp_session_env_flag(const char* value) {
    if (!value) return -1;
    return (*value && *value != '0') ? 1 : 0;
}

#endif
