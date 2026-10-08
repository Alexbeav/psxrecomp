/* A first AV difference must not hide a later EXT difference when END RAM
 * and cycle still match. Reuse the deterministic stand-in, not guest data.
 * The qualification packet also builds this with the unchanged base fixture. */
#define main unused_replay_fixture_main
#define replay_host_state_digest replay_standin_state_digest
#ifndef REPLAY_SESSION_FIXTURE_PATH
#define REPLAY_SESSION_FIXTURE_PATH "test_replay_session.c"
#endif
#include REPLAY_SESSION_FIXTURE_PATH
#undef main
#undef replay_host_state_digest
#ifdef REPLAY_FIXTURE_OLD
int replay_host_capture(const char *path) { (void)path; return 0; }
#endif
static int ext_difference;
int replay_host_state_digest(uint32_t out[4]) {
    int ok = replay_standin_state_digest(out);
    if (ok && ext_difference) out[3] ^= 1;
    return ok;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    snprintf(dir, sizeof dir, "%s", argv[1]);
    mkdir_p(dir);
    snprintf(verdict_path, sizeof verdict_path, "%s/verdict.json", dir);
    replay_session_set_verdict_path(verdict_path);
    clear_slots();
    memset(ram, 0, sizeof ram); cycle = 1000; vram_word = 0;
    record(180, "cd_speed=1\n");
    CHECK(replay_session_play_slot(0), "start AV then EXT control");
    drift_av = 1;
    for (unsigned i = 0; i < 85; ++i) vblank(0xffff, neutral);
    drift_av = 0; ext_difference = 1;
    for (unsigned i = 0; i < 110 && replay_session_state() != REPLAY_IDLE; ++i) vblank(0xffff, neutral);
    ext_difference = 0;
    CHECK(replay_session_last_result() == REPLAY_RESULT_OUT_OF_SYNC,
          "later EXT differs with matching END RAM/cycle (result %d)", replay_session_last_result());
    CHECK(read_verdict() && strstr(verdict, "\"differing_ram_pages\": 0") &&
          strstr(verdict, "\"divergence_parts\": \"av\""),
          "first difference stays AV, END RAM still matches: %s", verdict);
    clear_slots();
    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: first AV then later EXT with matching END RAM/cycle, %d checks\n", checks);
    return 0;
}
