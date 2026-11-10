#!/usr/bin/env python3
"""Guard that a seat that is not a pad cannot start a netplay match (PS1B-313).

A match carries pad input only. A Mouse or GunCon seat sat in the match with
no input, a neGcon lost its twist, and nothing told the player. main.cpp now
refuses at the one place a session starts. The sentence is tested in
test_netplay_exit_reason.c; a match needs sockets and a second peer, so that
main.cpp asks, stops a CLI start, and takes a lobby match back to the room is
checked in the source.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

starts = [m.start() for m in re.finditer(r"psx_netplay_start\(&net_cfg\)", MAIN)]
assert len(starts) == 1, "main.cpp must start a netplay session in exactly one place"
start = starts[0]

# The question is asked of the seat that feeds this player's input, by the
# device that seat presents, before the session is opened.
asked = MAIN.rfind("netplay_seat_refusal(", 0, start)
assert asked >= 0, "the session start does not ask whether the seat is a pad"
question = MAIN[asked:start]
assert "sio_device_for_player(g_players[net_cfg.input_player])" in question, (
    "the refusal must look at the seat that feeds this player's input"
)
assert "net_cfg.input_player + 1" in question, "the sentence must name the port as the player counts it"
resolved = MAIN.rfind("net_cfg.input_player = ", 0, asked)
assert resolved >= 0, "the input seat must be resolved before it is checked"

# A CLI start stops before the session is opened; the sentence goes to stderr.
assert "if (netplay_seat_refused) {" in question, "nothing acts on the refusal before the session start"
acts = question[question.index("if (netplay_seat_refused) {") :]
assert "std::fprintf(stderr" in acts and "netplay_seat_why" in acts, "the sentence is not printed"
# The stop goes through refuse_start, which records the sentence for the run
# report (PS1G-63) and returns 1 when no other code is given.
assert re.search(
    r'if \(!g_netplay_from_lobby\)\s*return refuse_start\("netplay_seat", "[^"]*",\s*netplay_seat_why\);',
    acts,
), "a CLI start must stop with exit code 1"
assert re.search(r"const std::string& sentence, unsigned seq_before,\s*int code = 1\) \{", MAIN), (
    "refuse_start must return 1 unless a code is given"
)

# A match from the lobby ends the session it opened and keeps the sentence for
# the status line, which the soft return to the lobby shows.
after = MAIN[start : start + 2000]
ends = after.index('netplay_soft_exit("netplay_seat_not_pad");')
assert "if (netplay_seat_refused) {" in after[:ends], "the lobby refusal is not conditional"
assert "g_netplay_exit_reason_text = netplay_seat_why;" in after[ends : ends + 160], (
    "the sentence must be kept for the launcher's status line after the soft exit"
)
assert "psx_lobby_set_last_error(g_netplay_exit_reason_text);" in MAIN, "the soft return no longer shows the reason"

# The guest is not entered after a refusal: the return to the lobby is taken
# before the scheduler runs.
runs = [m.start() for m in re.finditer(r"(?m)^\s*psx_scheduler_run\(&cpu\);", MAIN)]
assert len(runs) == 1, "expected one scheduler entry"
before = MAIN[runs[0] - 260 : runs[0]]
assert re.search(
    r"if \(psx_return_to_lobby_requested\(\) && g_netplay_from_lobby\)\s*goto soft_return_lobby;\s*$", before
), "a refused match would boot the game offline: no return to the lobby before the scheduler"

print("netplay pad seat wiring test: PASS")
