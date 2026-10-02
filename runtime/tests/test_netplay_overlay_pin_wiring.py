#!/usr/bin/env python3
"""Guard that a netplay session runs overlays interpreted (PS1B-367).

Native and interpreted overlay code do not count the same guest cycles, so a
pair of peers leaves sync at the first overlay one runs native and the other
interprets. main.cpp therefore holds the overlay native tier off for the
length of a session, with the same counted pin a replay holds. A netplay
session needs sockets and a second peer, so the wiring is checked in the
source, as test_netplay_lan_identity_wiring.py does for the lobby.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")


def body(signature: str) -> str:
    """The text of the function that starts at `signature`, to its closing brace."""
    start = MAIN.find(signature)
    assert start >= 0, f"main.cpp has no function: {signature}"
    depth = 0
    for i in range(MAIN.index("{", start), len(MAIN)):
        if MAIN[i] == "{":
            depth += 1
        elif MAIN[i] == "}":
            depth -= 1
            if depth == 0:
                return MAIN[start : i + 1]
    raise AssertionError(f"unterminated body: {signature}")


# The counted pin: the first holder turns the native tier off and keeps what it
# was; the last holder to let go puts that back.
acquire = body("static void overlay_interp_pin_acquire(void) {")
assert "s_overlay_interp_holders++ == 0" in acquire, "only the first holder may save and switch the tier"
assert acquire.index("overlay_loader_get_native_exec()") < acquire.index("overlay_loader_set_native_exec(0)"), (
    "the tier's state must be saved before it is turned off"
)
release = body("static void overlay_interp_pin_release(void) {")
assert "--s_overlay_interp_holders > 0" in release, "the tier must stay off while another holder remains"
assert "overlay_loader_set_native_exec(s_overlay_native_saved)" in release, "the last release must restore the saved state"

# The session's pin and its release each act once per session.
pin = body("static void netplay_overlay_pin(void) {")
assert "overlay_interp_pin_acquire();" in pin and "s_netplay_overlay_pinned = true;" in pin
assert pin.index("if (s_netplay_overlay_pinned") < pin.index("overlay_interp_pin_acquire();"), (
    "a second pin in one session must not take a second hold"
)
unpin = body("static void netplay_overlay_unpin(void) {")
assert "overlay_interp_pin_release();" in unpin and "s_netplay_overlay_pinned = false;" in unpin
assert unpin.index("if (!s_netplay_overlay_pinned)") < unpin.index("overlay_interp_pin_release();"), (
    "shutdown runs more than once per match; only the first may release"
)

# Taken where the session starts, after the start succeeded and before anything
# else of the session is set up. Lobby, Direct IP and LAN matches, and a
# rematch after a return to the lobby, all start through this one call.
starts = [m.start() for m in re.finditer(r"psx_netplay_start\(&net_cfg\)", MAIN)]
assert len(starts) == 1, "main.cpp must start a netplay session in exactly one place"
after_start = MAIN[starts[0] : starts[0] + 900]
assert "netplay_overlay_pin();" in after_start, "the session start does not take the overlay pin"
assert after_start.index("return 1;") < after_start.index("netplay_overlay_pin();") < after_start.index(
    "apply_netplay_local_viewport_aspect("
), "the pin must follow the failed-start return and come before the session is set up"

# Given back at every place the session is shut down: the soft exit (a
# mismatch, a disconnect, a stall, the player leaving), the return to the
# lobby, and process shutdown.
shutdowns = [m.start() for m in re.finditer(r"(?m)^\s*psx_netplay_shutdown\(\);", MAIN)]
assert len(shutdowns) >= 3, "expected the soft exit, the lobby teardown and the process shutdown"
for at in shutdowns:
    following = MAIN[at : at + 160]
    assert "netplay_overlay_unpin();" in following, (
        "a psx_netplay_shutdown() call is not followed by netplay_overlay_unpin(): " + MAIN[at : at + 80].strip()
    )
for signature in (
    "static void netplay_soft_exit(const char *origin) {",
    "static void shutdown_runtime(void) {",
    "static void teardown_game_session_keep_lobby(void) {",
):
    assert "netplay_overlay_unpin();" in body(signature), f"no overlay unpin in: {signature}"

# A replay holds the same counted pin, so a replay and a session cannot undo
# each other.
replay_pin = body("static void replay_overlay_pin(void) {")
assert "overlay_interp_pin_acquire();" in replay_pin, "the replay pin must use the counted pin"
assert "overlay_loader_set_native_exec(" not in replay_pin, "the replay pin must not switch the tier by itself"
replay_unpin = body("static void replay_overlay_unpin_if_idle(void) {")
assert "overlay_interp_pin_release();" in replay_unpin, "the replay unpin must use the counted pin"
assert "overlay_loader_set_native_exec(" not in replay_unpin, "the replay unpin must not switch the tier by itself"

print("netplay overlay pin wiring test: PASS")
