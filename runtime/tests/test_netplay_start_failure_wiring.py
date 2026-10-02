#!/usr/bin/env python3
"""Guard that a failed netplay start says what failed (PS1B-386).

The start printed "built without recomp-net, or bind/peer invalid" for every
failure. A player whose system held the UDP port read first that the build
had no netplay, and a match started from the launcher closed the program with
no text. main.cpp now tries the bind itself, reports the system's answer, and
takes a launcher match back to the room with the sentence. The sentences are
tested in test_netplay_exit_reason.c; a refused bind needs a held port and the
launcher path needs a room, so that main.cpp probes, prints, stops a CLI start
and returns a launcher match is checked in the source.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

# The old sentence, which named the build for every failure, is gone.
assert "built without recomp-net, " not in MAIN, "the start failure still blames the build first"

starts = [m.start() for m in re.finditer(r"psx_netplay_start\(&net_cfg\)", MAIN)]
assert len(starts) == 1, "main.cpp must start a netplay session in exactly one place"
after = MAIN[starts[0] : starts[0] + 700]

# A failed start asks for the sentence, stops a CLI start with code 1, and
# takes a launcher match back to the room with the sentence kept for the
# status line.
failed = after.index("if (nrc != 0) {")
block = after[failed : after.index("} else {", failed)]
assert "netplay_start_failure(nrc, net_cfg)" in block, "the failure is not explained"
assert re.search(r"if \(!g_netplay_from_lobby\)\s*return 1;", block), "a CLI start must stop with exit code 1"
ends = block.index('netplay_soft_exit("netplay_start_failed");')
assert block.index("return 1;") < ends, "a CLI start must stop before the return to the room"
assert "g_netplay_exit_reason_text = why;" in block[ends:], (
    "the sentence must be kept for the launcher's status line after the soft exit"
)
assert "psx_lobby_set_last_error(g_netplay_exit_reason_text);" in MAIN, "the soft return no longer shows the reason"
# The session-only steps run only when the session started.
started = after[after.index("} else {", failed) :]
assert "netplay_overlay_pin();" in started, "the overlay pin must be taken only after a successful start"

# The guest is not entered after a failed launcher start: the return to the
# lobby is taken before the scheduler runs.
runs = [m.start() for m in re.finditer(r"(?m)^\s*psx_scheduler_run\(&cpu\);", MAIN)]
assert len(runs) == 1, "expected one scheduler entry"
assert re.search(
    r"if \(psx_return_to_lobby_requested\(\) && g_netplay_from_lobby\)\s*goto soft_return_lobby;\s*$",
    MAIN[runs[0] - 260 : runs[0]],
), "a failed launcher start would boot the game offline: no return to the lobby before the scheduler"

# The explanation: the bind is tried again for a LAN start only, the system's
# answer goes into the sentence and the log line, and the build is named from
# the build's own definition.
helper = MAIN[MAIN.index("static const char* netplay_start_failure(int nrc") :]
helper = helper[: helper.index("\n}\n") + 3]
assert re.search(r"#if defined\(PSX_HAS_RECOMP_NET\)\s*const int netplay_built = 1;\s*#else\s*const int netplay_built = 0;",
                 helper), "whether the build has netplay must come from the build"
assert re.search(r"\(netplay_built && nrc == -3\)\s*\? netplay_bind_probe\(cfg\.bind_hostport, &sys_error, sys_text, sizeof\(sys_text\)\)\s*"
                 r": NETPLAY_BIND_NOT_TRIED;", helper), "the bind must be tried again for a LAN start, and only then"
assert "netplay_start_failure_text(nrc, netplay_built, cfg.bind_hostport," in helper, "the sentence is not built"
assert "std::fprintf(stderr" in helper and "netplay start failed (%d)" in helper and "[system: " in helper, (
    "the log line must carry the sentence and the system's text"
)

# The probe binds without address reuse, on both platforms, and keeps the
# system's error number.
probe = MAIN[MAIN.index("static int netplay_bind_probe(") : MAIN.index("static const char* netplay_start_failure(int nrc")]
assert "SO_REUSEADDR" not in probe.split("*/", 1)[1], "the probe must not ask for address reuse: a held port would go unseen"
assert "WSAGetLastError()" in probe and "errno" in probe, "the system's error number is not read on both platforms"
assert probe.count("bind(s, (const sockaddr*)&addr, sizeof(addr))") == 2, "the probe must bind on both platforms"
assert "closesocket(s);" in probe and "close(s);" in probe, "the probe must close its socket"
assert "return NETPLAY_BIND_NOT_TRIED;" in probe, "a host name must be left to the library"

print("netplay start failure wiring test: PASS")
