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
# The stop goes through refuse_start, which records the sentence for the run
# report (PS1G-63) and returns 1 when no other code is given.
stop = re.search(
    r'if \(!g_netplay_from_lobby\)\s*return refuse_start_reported\("netplay_start", "[^"]*",\s*why, s_netplay_start_report\);', block
)
assert stop, "a CLI start must stop with exit code 1, with the sentence"
reported = MAIN[MAIN.index("static int refuse_start_reported(const char* kind, const char* title,"):]
reported = reported[: reported.index("\n}\n") + 3]
assert "return 1;" in reported, "the helper for a start with an address in its sentence must return 1"
ends = block.index('netplay_soft_exit("netplay_start_failed");')
assert stop.start() < ends, "a CLI start must stop before the return to the room"
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
assert re.search(r"\(netplay_built && nrc == -3\)\s*\? netplay_bind_probe\(cfg\.bind_hostport, cfg\.peer_hostport, tried, sizeof\(tried\),\s*"
                 r"&sys_error, sys_text, sizeof\(sys_text\)\)\s*: NETPLAY_BIND_NOT_TRIED;", helper), (
    "the bind must be tried again for a LAN start, and only then"
)
# The sentence names the address that was bound, and gets the system's number and text.
assert re.search(r"tried\[0\] \? tried : cfg\.bind_hostport,\s*cfg\.peer_hostport, bind_probe, sys_error, sys_text,", helper), (
    "the sentence must get the bound address, the probe result and the system's answer"
)
assert "netplay_start_failure_text(nrc, netplay_built," in helper, "the sentence is not built"
assert "std::fprintf(stderr" in helper and "netplay start failed (%d)" in helper and "[system: " in helper, (
    "the log line must carry the sentence and the system's text"
)

# The probe binds without address reuse, on both platforms, and keeps the
# system's error number.
probe = MAIN[MAIN.index("static int netplay_bind_probe(") : MAIN.index("static const char* netplay_start_failure(int nrc")]
assert "SO_REUSEADDR" not in probe, "the probe must not ask for address reuse: a held port would go unseen"
assert "WSAGetLastError()" in probe and "errno" in probe, "the system's error number is not read on both platforms"
assert probe.count("bind(s, (const sockaddr*)&addr, sizeof(addr))") == 2, "the probe must bind on both platforms"
assert "closesocket(s);" in probe and "close(s);" in probe, "the probe must close its socket"
assert probe.count("result = NETPLAY_BIND_NO_SOCKET;") == 2, "a socket that could not be made is not a refused port"

# A sentence says a cause only when the probe found it.
# The listen address is read the way the library reads it: a name is looked up, so a held port behind a name
# is found, and no address before the colon means every address. The probe never skips the bind.
reader = MAIN[MAIN.index("static int netplay_hostport_addr(") : MAIN.index("/* Why a LAN netplay start failed")]
assert "getaddrinfo(host, nullptr, &hints, &found)" in reader, "a host name must be looked up before the bind"
assert re.search(r'if \(!host\[0\] \|\| std::strcmp\(host, "0\.0\.0\.0"\) == 0\) \{\s*out->sin_addr\.s_addr = htonl\(INADDR_ANY\);',
                 reader), "no address before the colon must mean every address, as in the library"
assert "NETPLAY_BIND_NOT_TRIED" not in probe, "the probe must not leave a listen address untried"
assert re.search(r"if \(netplay_hostport_addr\(bind_hostport, &addr\) != 0\)\s*return NETPLAY_BIND_BAD_ADDRESS;", probe), (
    "a listen address the library refuses must be reported as such"
)
# The peer address is read only after the listen address opened, and is named only when it cannot be read.
opened = probe.index("if (result != NETPLAY_BIND_OK)")
peer = probe.index("netplay_hostport_addr(peer_hostport, &peer) != 0 || peer.sin_port == 0")
assert opened < peer < probe.index("return NETPLAY_BIND_PEER_BAD;"), "the peer address is judged before the listen address"

REASON = (ROOT / "runtime" / "src" / "netplay_exit_reason.c").read_text(encoding="utf-8")
text = REASON[REASON.index("void netplay_start_failure_text(") :]
text = text[: text.index("\n}\n")]
# Each cause the system names has its own sentence, chosen by the number; any other number gets the neutral one.
order = [
    "bind_error_kind(sys_error) == BIND_ERROR_NOT_ALLOWED",
    "The operating system does not allow this port.",
    "bind_error_kind(sys_error) == BIND_ERROR_IN_USE",
    "holds that ",
    "bind_error_kind(sys_error) == BIND_ERROR_NO_SUCH_ADDR",
    "address is not one of this computer's addresses.",
    "start_rc == -3 && bind_probe == NETPLAY_BIND_FAILED) {",
    'Netplay could not open UDP port %.5s on %.15s %s."',
    "bind_probe == NETPLAY_BIND_PEER_BAD",
    "the other player's address ",
]
at = [text.index(piece) for piece in order]
assert at == sorted(at), "the start-failure sentences are not chosen in the expected order"
assert text.count("the other player's address ") == 1, "the peer address is blamed in more than one case"
assert re.search(r"case 10048: case 98: case 48:\s*return BIND_ERROR_IN_USE;", REASON), "address in use: Windows, Linux, macOS"
assert re.search(r"case 10013: case 13:\s*return BIND_ERROR_NOT_ALLOWED;", REASON), "access denied: Windows, POSIX"
assert re.search(r"case 10049: case 99: case 49:\s*return BIND_ERROR_NO_SUCH_ADDR;", REASON), (
    "address not available: Windows, Linux, macOS"
)
# The online failure names no log and no setting: the code does not say which of its causes it was.
online = text[text.index("start_rc == -4") :]
online = online[: online.index("} else {")]
assert "The log" not in online and "setting" not in online.split("*/")[-1], "the online sentence says more than the code knows"

print("netplay start failure wiring test: PASS")
