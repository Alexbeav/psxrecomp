#!/usr/bin/env python3
"""Guard that the replay identity follows the disc in the drive (PS1B-316).

test_replay_disc_identity.c proves what a replay names and plays on once the
product identity is told about a mounted disc. This proves the host tells it:
both mounts that happen under a running game go through one helper, a save
state cannot mount a disc while a replay records or plays, and a boot
recording does not start when a save state is staged for boot. The mount
paths open disc images and sit in the middle of main.cpp, out of reach of a
unit test, so the wiring is checked in the source, as
test_negcon_twist_wiring.py does for the neGcon.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "runtime" / "src" / "input_route_session.c").read_text(encoding="utf-8")


def body(text: str, signature: str) -> str:
    """The text of the function that starts at `signature`, to its closing brace."""
    start = text.index(signature)
    depth = 0
    for i in range(text.index("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise AssertionError(f"unterminated body: {signature}")


# The session call drops both cached digests and leaves the BIOS alone.
set_disc = body(SESSION, "void input_route_session_set_disc(")
for needed in ("s_product_serial", "s_product_disc", "s_digest_cached = 0", "s_replay_digest_cached = 0"):
    assert needed in set_disc, f"input_route_session_set_disc lost: {needed}"
assert "bios" not in set_disc, "a disc change must not touch the BIOS identity"

# One helper: the identity under the mutex, then the serial used for names.
helper = body(MAIN, "static void replay_identity_follow_disc(const std::string& serial,\n"
              "                                        const std::string& mount) {")
lock = helper.index("s_replay_identity_mutex")
call = helper.index("input_route_session_set_disc(serial.c_str(), mount.c_str())")
assert lock < call, "the identity changes under s_replay_identity_mutex"
assert "s_replay_disc_serial = serial;" in helper
# A hash of the previous disc is stopped before the frame waits on its thread.
cancel, join = helper.index("disc_digest_cache_cancel(1)"), helper.index("s_replay_digest_thread.join()")
assert cancel < join < helper.index("disc_digest_cache_cancel(0)") < lock, (
    "replay_identity_follow_disc must cancel, join, re-enable, then set the disc"
)

# Nothing else changes the disc identity under a running game.
uses = re.findall(r"\binput_route_session_set_disc\(", MAIN)
assert len(uses) == 1, f"input_route_session_set_disc is called outside the helper ({len(uses)} uses)"
calls = re.findall(r"^\s+replay_identity_follow_disc\(", MAIN, re.M)
assert len(calls) == 2, f"expected the two mount points to call the helper, found {len(calls)}"

# 1. The in-game disc change: after the drive took the disc, with the picked
#    image's own serial and the path that was mounted.
change = body(MAIN, "static int runtime_ui_change_disc(")
replaced = change.index("if (!cdrom_replace_disc(mount.c_str(), scex_ptr)) {")
followed = change.index("replay_identity_follow_disc(identity.detected_serial, mount);")
assert replaced < followed, "the identity follows only a successful cdrom_replace_disc"
assert "return 0;" in change[replaced:followed], "a failed replace must return before the identity update"
# A replay in progress ends before the swap, so no replay spans two discs.
assert change.index("replay_session_shutdown();") < replaced

# 2. A save state that mounted its own disc: when the mount is kept.
result = body(MAIN, 'extern "C" void psx_frontend_savestate_mount_result(')
kept = result.index("if (!kept) return;")
assert kept < result.index("replay_identity_follow_disc(g_restore_mount_serial,"), (
    "the identity follows a restore mount only when it is kept"
)

# 3. The gate: no restore mount while a replay records or plays, and it is
#    decided before the drive is touched.
mount = body(MAIN, 'extern "C" int psx_frontend_savestate_mount_disc(')
gate = re.search(
    r"if \(replay_session_state\(\) != REPLAY_IDLE\) \{\s*"
    r"std::snprintf\(why, why_cap,\s*"
    r'"the disc cannot change while a replay records or plays"\);\s*return 0;\s*\}',
    mount,
)
assert gate, "psx_frontend_savestate_mount_disc lost its replay gate"
begin = mount.index("cdrom_restore_mount_begin(")
assert gate.end() < begin, "the replay gate must come before cdrom_restore_mount_begin"
# The serial handed to the helper is the mounted image's own boot serial.
assert re.search(
    r"g_restore_mount_image = resolved\.mount;\s*g_restore_mount_serial = id\.detected_serial;\s*return 1;",
    mount[begin:],
), "the restore mount must keep the mounted image's serial for the identity"

# 4. A boot recording starts only at a cold boot: a save state staged for
#    boot (PSX_LOAD_SLOT) could mount another disc under a power-on replay.
power_on = body(MAIN, 'extern "C" int replay_host_at_power_on(void)')
assert 'std::getenv("PSX_LOAD_SLOT") != nullptr' in power_on
assert re.search(r"return s_replay_vblank == 0 && !load_slot;", power_on), (
    "replay_host_at_power_on must refuse when PSX_LOAD_SLOT is set"
)

print("replay disc identity wiring: ok")
