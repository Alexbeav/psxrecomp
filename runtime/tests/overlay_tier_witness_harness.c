/* Authored loader controls; reuse the existing authored callback stubs. */
#define main pair_fixture_main
#define psx_get_cycle_count pair_fixture_cycle
#include "overlay_pair_dedup_harness.c"
#undef main
#undef psx_get_cycle_count
static uint64_t s_test_cycle;
uint64_t psx_get_cycle_count(void) { return s_test_cycle; }

/* Include the real loader to inject a broken invariant without adding a
 * production mutation API. All ordinary admission/dispatch uses its public API. */
#define PSX_OVERLAY_DLL_BUILD 1
#include "../src/overlay_loader.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    CHECK(argc == 5);
    const char *mode = argv[2], *first = argv[3], *second = argv[4];
    overlay_loader_init(argv[1], "PAIR-TEST", 0);
    CHECK(overlay_loader_registered_count() == 2);
    CHECK(reveal_second_pair(second));
    OverlayPreparedImage *prepared = overlay_loader_prepare_published(second);
    CHECK(prepared != NULL);
    int saved_native = strcmp(mode, "prior-off") != 0;
    int saved_freeze = !saved_native || strcmp(mode, "selfcheck-end") == 0;
    overlay_loader_set_native_exec(saved_native);
    overlay_loader_set_load_freeze(saved_freeze);
    s_frame_count = 100; s_test_cycle = 1234;
    CHECK(overlay_loader_tier_hold(OVERLAY_TIER_REPLAY));
    CHECK(overlay_loader_tier_hold(OVERLAY_TIER_REPLAY));
    CHECK(overlay_loader_tier_hold(OVERLAY_TIER_NETPLAY));
    CHECK(overlay_loader_get_native_exec() == 0);
    CHECK(overlay_loader_load_frozen() == 1);
    overlay_loader_set_native_exec(1);
    if (strcmp(mode, "prior-off") != 0) overlay_loader_set_load_freeze(0);
    if (strcmp(mode, "selfcheck-end") == 0) saved_freeze = 0;
    if (strcmp(mode, "selfcheck-begin") == 0) {
        overlay_loader_set_load_freeze(1);
        saved_freeze = 1;
    }
    CHECK(overlay_loader_get_native_exec() == 0);
    CHECK(overlay_loader_load_frozen() == 1);
    /* Frozen commit still consumes/discards its mapped image, not retention (b). */
    CHECK(overlay_loader_commit_published(prepared) == 0);
    CHECK(!module_is_loaded(second));
    overlay_loader_rescan();
    CHECK(!module_is_loaded(second));
    CHECK(counter_value(second, "test_init_count") == 0);
    CPUState cpu = {0};
    s_in_shadow = 1;
    s_shadow_cand = &s_cand[0]; /* the own-continuation bypass must stay protected */
    for (int i = 0; i < 5; ++i)
        CHECK(overlay_loader_dispatch(&cpu, 0x80010000u) == 0);
    s_shadow_cand = NULL;
    s_in_shadow = 0;
    CHECK(counter_value(first, "test_call_count") == 0);
    OverlayTierWitness w;
    overlay_loader_tier_witness(&w);
    CHECK(w.holders[0] == 2 && w.holders[1] == 1);
    CHECK(w.begin_candidates == 2 && w.candidates == 2 && w.dlls == 1);
    CHECK(w.native_entries == 0 && w.violations == 0);
    CHECK(w.admission_checks > 0 && w.blocked_admissions == w.admission_checks);
    CHECK(w.dispatch_checks > 0 && w.blocked_dispatches == w.dispatch_checks);
    if (strcmp(mode, "overflow") == 0) {
        CHECK(!overlay_loader_tier_hold(OVERLAY_TIER_REPLAY));
        overlay_loader_tier_witness(&w);
        CHECK(w.overflow == 1 && w.holders[0] == 2);
        CHECK(w.dispatch_checks == 3); /* saturates rather than wrapping */
    }
    if (strcmp(mode, "violation") == 0) {
        s_cand_n++; /* authored fault: silent admission bypass */
        s_frame_count = 101; s_test_cycle = 1250;
        CHECK(overlay_loader_dispatch(&cpu, 0x80010000u) == 0);
        s_cand_n--;
        s_frame_count = 102; s_test_cycle = 1260;
        s_native_exec = 1; /* a later fault must not replace the first coordinate */
        CHECK(overlay_loader_dispatch(&cpu, 0x80010000u) == 0);
        s_native_exec = 0;
        overlay_loader_tier_witness(&w);
        CHECK(w.violations == 3 && w.first_violation_frame == 101);
        CHECK(w.first_violation_cycle == 1250 && w.first_violation_pc == 0x10000);
    }
    overlay_loader_tier_release(OVERLAY_TIER_NETPLAY);
    overlay_loader_tier_release(OVERLAY_TIER_NETPLAY); /* repeated shutdown */
    overlay_loader_tier_release(OVERLAY_TIER_REPLAY);
    CHECK(overlay_loader_get_native_exec() == 0 && overlay_loader_load_frozen());
    s_frame_count = 200; s_test_cycle = 2345;
    overlay_loader_tier_release(OVERLAY_TIER_REPLAY);
    overlay_loader_tier_release(OVERLAY_TIER_REPLAY);
    CHECK(overlay_loader_get_native_exec() == saved_native);
    CHECK(overlay_loader_load_frozen() == saved_freeze);
    overlay_loader_tier_witness(&w);
    CHECK(w.holders[0] == 0 && w.holders[1] == 0 && w.intervals == 1);
    CHECK(w.begin_frame == 100 && w.begin_cycle == 1234);
    CHECK(w.end_frame == 200 && w.end_cycle == 2345);
    CHECK(w.begin_candidates == w.end_candidates && w.begin_dlls == w.end_dlls);
    CHECK(w.begin_native_entries == w.end_native_entries);
    /* State after the protected interval may grow without changing that evidence. */
    overlay_loader_set_load_freeze(0);
    prepared = overlay_loader_prepare_published(second);
    CHECK(prepared != NULL && overlay_loader_commit_published(prepared) == 2);
    overlay_loader_tier_witness(&w);
    CHECK(w.candidates == 4 && w.dlls == 2 && w.end_candidates == 2);
    overlay_loader_set_native_exec(1);
    CHECK(overlay_loader_dispatch(&cpu, 0x80010000u) == 1);
    overlay_loader_tier_witness(&w);
    CHECK(w.native_entries == 1 && w.end_native_entries == 0);
    char json[2048], tiny[8] = "dirty";
    CHECK(!overlay_loader_tier_witness_json(tiny, sizeof(tiny)) && tiny[0] == 0);
    CHECK(overlay_loader_tier_witness_json(json, sizeof(json)) > 0);
    puts(json);
    return 0;
}
