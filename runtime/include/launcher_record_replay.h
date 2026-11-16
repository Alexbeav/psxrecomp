// launcher_record_replay.h - the launcher's "Record replay" choice (PS1B-316).
//
// Ticked, the next boot records a power-on replay (replay_session.h) from the
// first vblank until the game exits. The choice is deliberately NOT persisted
// in settings.toml: a recording runs overlays interpreted and writes a file
// every session, so a tick left on from an earlier session must not quietly
// slow down and fill every later one. It lives for the process instead: the
// host seeds the launcher with it and reads it back. A launcher that predates
// the field (no RECOMP_LAUNCHER_HAS_RECORD_REPLAY) has no checkbox, and the
// host then never records unless PSX_REPLAY_RECORD_BOOT=1 asks for it.

#pragma once

#include <cstring>

namespace PSXRecompV4 {

// The launcher field for this process's current choice.
inline int launcher_record_replay_seed(bool requested) {
    return requested ? 1 : 0;
}

// The choice after the launcher returns. PLAY (result 0) takes the box as the
// player left it; any other result (Quit exits, a relaunch starts a new
// process) keeps the previous choice.
inline bool launcher_record_replay_readback(int launcher_value, int launcher_result,
                                            bool previous) {
    return launcher_result == 0 ? launcher_value > 0 : previous;
}

// Whether this boot records a power-on replay: the launcher's choice, or
// PSX_REPLAY_RECORD_BOOT=1 for headless and scripted runs.
inline bool record_boot_replay_requested(bool launcher_choice, const char* env) {
    return launcher_choice || (env && std::strcmp(env, "1") == 0);
}

} // namespace PSXRecompV4
