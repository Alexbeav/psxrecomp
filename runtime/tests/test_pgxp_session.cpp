/* test_pgxp_session.cpp -- does the psx.enhancement.pgxp mod arm PGXP?
 *
 * The mod's activation runs at session start; main.cpp's renderer setup runs
 * after it and applies the [video] baseline. Before docs/ENHANCEMENTS.md G1.11
 * the activation armed the corrections directly and the baseline switched
 * them straight back off, so the mod did nothing. Now the activation records
 * a request (pgxp_mod_request) and the renderer setup combines the two through
 * psx_pgxp_session_resolve (pgxp_session.h). This test drives the real
 * activation callback (src/mod_builtin_pgxp.c, registered through a stubbed
 * mod API) and the real resolver, in the order main.cpp runs them:
 *
 *   reset_mod_owned_presentation()  -> pgxp_mod_request(0, 0)
 *   mod_runtime_activate_plugins()  -> builtin_pgxp_activate (if planned)
 *   renderer setup                  -> psx_pgxp_session_resolve(...)
 */
#include "pgxp.h"
#include "pgxp_session.h"
#include "mod_plugins.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- stubs: the mod API the builtin plugin calls ------------------------ */

static PSXModActivationCallback g_activation = nullptr;
static std::string g_activation_id;
static std::string g_cpu_mode_option = "false";

extern "C" int psx_mod_register_activation_plugin(
        const char* id, PSXModActivationCallback callback) {
    g_activation_id = id ? id : "";
    g_activation = callback;
    return 1;
}

extern "C" int psx_mod_option_value(const char* package_id,
                                    const char* feature_id,
                                    const char* option_id, char* out,
                                    uint32_t out_size) {
    if (std::strcmp(package_id, "psx.enhancement.pgxp") != 0 ||
        std::strcmp(feature_id, "pgxp") != 0 ||
        std::strcmp(option_id, "cpu_mode") != 0 || out_size == 0)
        return 0;
    std::snprintf(out, out_size, "%s", g_cpu_mode_option.c_str());
    return 1;
}

/* pgxp.cpp's position-cache tier lives in gte.cpp. */
extern "C" int gte_geometry_correction_lookup(uint32_t, int32_t*, int32_t*) {
    return 0;
}

/* ---- one session start, as main.cpp sequences it ------------------------ */

struct Baseline {
    int geometry = 0, texture = 0, cpu_mode = 0;
    const char* env_geometry = nullptr;
    const char* env_texture = nullptr;
    const char* env_cpu_mode = nullptr;
};

static PSXPgxpSessionArm run_session(bool mod_planned, const Baseline& b) {
    pgxp_mod_request(0, 0);                       /* reset_mod_owned_presentation */
    if (mod_planned && g_activation) g_activation(); /* mod_runtime_activate_plugins */
    PSXPgxpSessionInputs in{};
    in.env_geometry = psx_pgxp_session_env_flag(b.env_geometry);
    in.env_texture = psx_pgxp_session_env_flag(b.env_texture);
    in.env_cpu_mode = psx_pgxp_session_env_flag(b.env_cpu_mode);
    in.video_geometry = b.geometry;
    in.video_texture = b.texture;
    in.video_cpu_mode = b.cpu_mode;
    in.mod_enabled = pgxp_mod_requested(&in.mod_cpu_mode);
    return psx_pgxp_session_resolve(&in);
}

static bool arm_is(const PSXPgxpSessionArm& a, int g, int t, int c) {
    return a.geometry == g && a.texture == t && a.cpu_mode == c;
}

int main(void) {
    /* The builtin registered itself at load, under the plugin id its
     * manifest names. */
    CHECK(g_activation != nullptr);
    CHECK(g_activation_id == "psx.pgxp");

    const Baseline off;   /* every [video] PGXP key at its default */

    /* Default: no mod, no keys -> the faithful floor. */
    CHECK(arm_is(run_session(false, off), 0, 0, 0));

    /* The enabled mod arms both corrections even though the baseline
     * applied after activation says off (the pre-G1.11 bug). */
    CHECK(arm_is(run_session(true, off), 1, 1, 0));

    /* Activation only records the request; it does not arm the engine
     * itself (the renderer setup does, from the resolved values). */
    pgxp_set_enabled(0);
    pgxp_mod_request(0, 0);
    g_activation();
    {
        int cpu = -1;
        CHECK(pgxp_mod_requested(&cpu) == 1 && cpu == 0);
        CHECK(pgxp_enabled() == 0);
    }

    /* The mod's CPU-mode option rides along. */
    g_cpu_mode_option = "true";
    CHECK(arm_is(run_session(true, off), 1, 1, 1));
    g_cpu_mode_option = "false";

    /* Netplay clears the plan: nothing activates, so the session keeps
     * exactly the baseline, even right after a session that had the mod. */
    CHECK(arm_is(run_session(true, off), 1, 1, 0));
    CHECK(arm_is(run_session(false, off), 0, 0, 0));
    {
        int cpu = -1;
        CHECK(pgxp_mod_requested(&cpu) == 0 && cpu == 0);
    }

    /* A player's [video] keys still work without the mod, and the mod adds
     * to them rather than replacing them. */
    {
        Baseline tex_only;
        tex_only.texture = 1;
        CHECK(arm_is(run_session(false, tex_only), 0, 1, 0));
        CHECK(arm_is(run_session(true, tex_only), 1, 1, 0));
        Baseline cpu_only;
        cpu_only.cpu_mode = 1;
        CHECK(arm_is(run_session(false, cpu_only), 0, 0, 1));
    }

    /* Validation env overrides win over both: an A/B arm can switch PGXP
     * off under an enabled mod, or on without one. */
    {
        Baseline env_off;
        env_off.env_geometry = "0";
        env_off.env_texture = "0";
        CHECK(arm_is(run_session(true, env_off), 0, 0, 0));
        Baseline env_on;
        env_on.env_geometry = "1";
        CHECK(arm_is(run_session(false, env_on), 1, 0, 0));
        Baseline env_empty;   /* set but empty = off, the runtime's old rule */
        env_empty.env_texture = "";
        CHECK(arm_is(run_session(true, env_empty), 1, 0, 0));
    }
    CHECK(psx_pgxp_session_env_flag(nullptr) == -1);
    CHECK(psx_pgxp_session_env_flag("0") == 0);
    CHECK(psx_pgxp_session_env_flag("") == 0);
    CHECK(psx_pgxp_session_env_flag("1") == 1);
    CHECK(psx_pgxp_session_env_flag("yes") == 1);

    if (g_failures) {
        std::fprintf(stderr, "test_pgxp_session: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("test_pgxp_session: all checks passed\n");
    return 0;
}
