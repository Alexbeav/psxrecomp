// PS1B-316: the launcher's "Record replay" choice between the launcher and
// the runtime (launcher_record_replay.h). Not persisted: it lives for the
// process, seeded into each launcher and read back after PLAY.
#include "launcher_record_replay.h"

#include <cassert>

int main() {
    using PSXRecompV4::launcher_record_replay_readback;
    using PSXRecompV4::launcher_record_replay_seed;
    using PSXRecompV4::record_boot_replay_requested;

    // Seed: the process's current choice.
    assert(launcher_record_replay_seed(false) == 0);
    assert(launcher_record_replay_seed(true) == 1);

    // PLAY (result 0) takes the box as the player left it.
    assert(launcher_record_replay_readback(1, 0, false));
    assert(!launcher_record_replay_readback(0, 0, true));
    assert(launcher_record_replay_readback(7, 0, false));
    assert(!launcher_record_replay_readback(-1, 0, true));
    // Quit (1), unavailable (2) and relaunch (3) keep the previous choice:
    // the launcher did not hand edits back.
    assert(!launcher_record_replay_readback(1, 1, false));
    assert(launcher_record_replay_readback(0, 2, true));
    assert(!launcher_record_replay_readback(1, 3, false));

    // Round trip: seed -> launcher leaves it -> PLAY -> same choice.
    for (int i = 0; i < 2; ++i) {
        const bool choice = i != 0;
        assert(launcher_record_replay_readback(launcher_record_replay_seed(choice), 0, !choice) == choice);
    }

    // A boot records when the launcher asked, or PSX_REPLAY_RECORD_BOOT=1.
    assert(!record_boot_replay_requested(false, nullptr));
    assert(record_boot_replay_requested(true, nullptr));
    assert(record_boot_replay_requested(false, "1"));
    assert(!record_boot_replay_requested(false, "0"));
    assert(!record_boot_replay_requested(false, ""));
    assert(!record_boot_replay_requested(false, "10"));
    assert(record_boot_replay_requested(true, "0"));
    return 0;
}
