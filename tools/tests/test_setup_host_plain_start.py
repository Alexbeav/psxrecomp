"""tools/setup_host_plain_start.py: a setup program is proven by starting it, not by asking it.

The Resident Evil 2 set package shipped with a setup program that exited before
its window opened (PS1B-365). `--setup-selfcheck` had passed on it: that answer
comes before the program reads its config. The plain start is the check that
would have failed, so it must fail on each way a start goes wrong and pass only
on the stamp that says the launcher is next.

Hermetic: the "setup program" is this file, run by the same Python, playing one
of four parts. No window, no SDL.
"""
import os
from pathlib import Path
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import setup_host_plain_start as tool  # noqa: E402

STAMP_LINE = "[boot-timing] +    0.5 ms  total   182.8 ms  host:before_run_window"


def play(part):
    """The stand-in setup program."""
    sys.stderr.write("psxrecomp: main() entered\n")
    sys.stderr.flush()
    if part == "starts":
        if os.environ.get("PSXRECOMP_NO_FORWARD") != "1" or os.environ.get("SDL_VIDEO_DRIVER") != "dummy" \
                or os.environ.get("PSX_HEADLESS") or os.environ.get("PSX_LAUNCHER_BOOT_TIMING") != "1":
            sys.stderr.write("the start environment is wrong\n")
            return 9
        sys.stderr.write(STAMP_LINE + "\n")
        sys.stderr.flush()
        time.sleep(60)                      # a real one would open its launcher now
        return 0
    if part == "refuses":
        sys.stderr.write('psxrecomp: failed to load --game D:\\x\\set.toml: [error] key "name" not found\n')
        sys.stderr.flush()
        time.sleep(60)                      # even when a message box keeps it alive
        return 1
    if part == "exits":
        return 0
    if part == "hangs":
        time.sleep(60)
        return 0
    return 2


def start(part, timeout=30.0):
    began = time.monotonic()
    verdict, text, lines = tool.plain_start([sys.executable, str(Path(__file__).resolve()), "--play", part],
                                            str(ROOT), timeout=timeout)
    return verdict, text, lines, time.monotonic() - began


class PlainStart(unittest.TestCase):
    def test_a_program_that_reaches_its_launcher_passes_and_is_stopped_at_once(self):
        os.environ["PSX_HEADLESS"] = "1"    # must not leak into the start
        try:
            verdict, text, lines, took = start("starts")
        finally:
            del os.environ["PSX_HEADLESS"]
        self.assertEqual(verdict, "pass", (text, lines))
        self.assertIn("reached host:before_run_window", text)
        self.assertLess(took, 20, "the program must be stopped at the stamp, not waited for")

    def test_a_program_that_refuses_its_config_fails_and_its_words_are_kept(self):
        verdict, text, lines, took = start("refuses")
        self.assertEqual(verdict, "fail")
        self.assertIn("refused its own config", text)
        self.assertTrue(any('key "name" not found' in line for line in lines), lines)
        self.assertLess(took, 20)

    def test_a_program_that_just_exits_fails(self):
        verdict, text, _lines, _took = start("exits")
        self.assertEqual(verdict, "fail")
        self.assertIn("exited with code 0 before host:before_run_window", text)

    def test_a_program_that_never_gets_there_fails_at_the_time_limit(self):
        verdict, text, _lines, took = start("hangs", timeout=2.0)
        self.assertEqual(verdict, "fail")
        self.assertIn("no host:before_run_window within 2 s", text)
        self.assertLess(took, 20)

    def test_a_file_that_cannot_be_started_is_said_not_passed(self):
        verdict, text, _lines = tool.plain_start([str(ROOT / "LICENSE")], str(ROOT), timeout=5.0)
        self.assertEqual(verdict, "cannot-run")
        self.assertEqual(tool.main(["--exe", str(ROOT / "LICENSE")]), tool.CANNOT_RUN)
        self.assertEqual(tool.main(["--exe", str(ROOT / "no-such-setup-program")]), 1)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--play":
        sys.exit(play(sys.argv[2]))
    unittest.main()
