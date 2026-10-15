"""Game entry keeps guest RAM 0..15; replays of older recordings still compare.

The C fixture links the real store path (memory.c), the real game-start latch
(fntrace.c) and the real core digest (netplay_state_digest.c). This driver
builds it at -O0 and -O2, and then reads the two places outside those sources
that the fixture cannot link: the replay host in main.cpp and the run report.
"""
import argparse
import re
import tempfile
from pathlib import Path

import source_fixture_link
from source_fixture_link import build_and_run

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"

# The fixture and memory.c compare bytes with memcmp, and Clang calls bcmp for
# a memcmp that is only tested for equality. A host without a static libc.a
# (Rocky 9) gives the link helper no list of the C library's functions; its
# fixed list lacks these two names, so it linked an aborting stub in their
# place and the fixture's first check stopped there.
source_fixture_link.ISO_C_FALLBACK |= {"memcmp", "bcmp"}


def body(text, signature):
    start = text.index(signature)
    depth = 0
    for at in range(text.index("{", start), len(text)):
        depth += {"{": 1, "}": -1}.get(text[at], 0)
        if not depth:
            return text[start:at + 1]
    raise AssertionError(signature + ": no closing brace")


def check_wiring():
    main = (SRC / "main.cpp").read_text(encoding="utf-8")
    capture = body(main, 'extern "C" void replay_host_settings_capture(')
    assert "game_entry_low_ram=kept\\n" in capture, \
        "a recording must say that this build keeps RAM 0..15 at game entry"
    apply_one = body(main, "static void replay_settings_apply_text(")
    assert re.search(r'key == "game_entry_low_ram"\) memory_set_low_ram_view_old\(value != "kept"\)',
                     apply_one), "the recording's line must switch the older-recording view"
    apply_all = body(main, 'extern "C" void replay_host_settings_apply(')
    assert apply_all.index("memory_set_low_ram_view_old(1)") < \
        apply_all.index("replay_settings_apply_text(settings"), \
        "a recording without the line is an older build's: switch the view on first"
    restore = body(main, 'extern "C" void replay_host_settings_restore(')
    assert "replay_settings_apply_text(s_replay_saved_settings" in restore, \
        "the end of playback must switch the view back with the saved settings"
    ram = body(main, 'extern "C" const uint8_t *replay_host_ram(')
    assert "memory_low_ram_view()" in ram, \
        "the replay's end checkpoint must read RAM 0..15 through the view"

    report = (SRC / "crash_trace.c").read_text(encoding="utf-8")
    assert "memory_low_ram_at_entry(low)" in report and '\\"low_ram_at_entry\\": ' in report, \
        "the run report must print low_ram_at_entry"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory() as root:
        for opt in ("-O0", "-O2"):
            build_and_run(args.cc, HERE, HERE.parent, opt, Path(root),
                          ["memory.c", "fntrace.c", "netplay_state_digest.c", "crc32.c"],
                          "test_low_ram_game_entry.c")
    print("PASS: game entry keeps RAM 0..15; the older-recording view is exact; "
          "its SWL/SWR limit is unchanged (O0/O2)")
