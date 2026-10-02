"""tools/setup_host_plain_start.py: a setup program is proven by starting it, not by asking it.

The Resident Evil 2 set package shipped with a setup program that exited before
its window opened (PS1B-365). `--setup-selfcheck` had passed on it: that answer
comes before the program reads its config. The plain start is the check that
would have failed, so it must fail on each way a start goes wrong and pass only
on the stamp that says the launcher is next.

Hermetic: the "setup program" is this file, run by the same Python, playing a
part. No window, no SDL, and no real setup program is started.

The start is closed (PS1B-406, PS1B-411): a real setup program can remove or
replace the machine's toolchain pack, so the tool gives it its own toolchain
and data folders. The parts here check that from the inside: the folders they
see are not the caller's, the toolchain variables are gone, and a part that
writes a pack or a pointer makes the start fail.
"""
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import setup_host_plain_start as tool  # noqa: E402

STAMP_LINE = "[boot-timing] +    0.5 ms  total   182.8 ms  host:before_run_window"
ROOT_NAMES = [name for name, _folder in tool.CLOSED_ROOTS]


def stamp_and_wait():
    sys.stderr.write(STAMP_LINE + "\n")
    sys.stderr.flush()
    time.sleep(60)                          # a real one would open its launcher now
    return 0


def play(part):
    """The stand-in setup program."""
    sys.stderr.write("psxrecomp: main() entered\n")
    sys.stderr.flush()
    if part == "starts":
        if os.environ.get("PSXRECOMP_NO_FORWARD") != "1" or os.environ.get("SDL_VIDEO_DRIVER") != "dummy" \
                or os.environ.get("PSX_HEADLESS") or os.environ.get("PSX_LAUNCHER_BOOT_TIMING") != "1":
            sys.stderr.write("the start environment is wrong\n")
            return 9
        return stamp_and_wait()
    if part == "closed":
        # Every root must be a folder of the start, not the caller's (given in CALLER_*), and no
        # variable may name a toolchain.
        wrong = []
        for name in ROOT_NAMES:
            value = os.environ.get(name, "")
            if not value or value == os.environ.get("CALLER_" + name, "") or "plain-start-" not in value \
                    or not os.path.isdir(value):
                wrong.append("%s=%s" % (name, value))
        for name in tool.TOOLCHAIN_VARIABLES:
            if os.environ.get(name):
                wrong.append("%s is set" % name)
        if os.environ.get(tool.READONLY_SWITCH) != "1":
            wrong.append("%s is not 1" % tool.READONLY_SWITCH)
        if os.environ.get("HTTPS_PROXY") != tool.DEAD_PROXY:
            wrong.append("HTTPS_PROXY=%s" % os.environ.get("HTTPS_PROXY"))
        if wrong:
            sys.stderr.write("the start is not closed: %s\n" % "; ".join(wrong))
            return 9
        return stamp_and_wait()
    if part in ("installs", "points"):
        # What a setup program's installer does: a pack in the cache, or only a pointer to one.
        cache = Path(os.environ["LOCALAPPDATA"]) / "retcomm" / "toolchains" / "cmake-clang-v1"
        if part == "installs":
            (cache / "1.0.14" / "bin").mkdir(parents=True)
            (cache / "1.0.14" / "bin" / "cmake.exe").write_text("x", encoding="utf-8")
        else:
            cache.mkdir(parents=True)
            (cache / "latest").write_text("pointer", encoding="utf-8")
        return stamp_and_wait()
    if part == "links":
        # The installer also links a pack into the package folder (its working folder).
        Path("toolchain").mkdir()
        Path("toolchain", ".psxrecomp-bin").write_text("somewhere\n", encoding="utf-8")
        return stamp_and_wait()
    if part == "marks":
        Path("started.txt").write_text("started", encoding="utf-8")
        return stamp_and_wait()
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


def start(part, timeout=30.0, cwd=None, sandbox_parent=None):
    began = time.monotonic()
    verdict, text, lines = tool.plain_start([sys.executable, str(Path(__file__).resolve()), "--play", part],
                                            str(cwd or ROOT), timeout=timeout, sandbox_parent=sandbox_parent)
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

    def test_the_program_gets_its_own_toolchain_and_data_folders(self):
        saved = dict(os.environ)
        try:
            for name in ROOT_NAMES:             # what the caller has, for the part to compare with
                os.environ["CALLER_" + name] = os.environ.get(name, "")
            for name in tool.TOOLCHAIN_VARIABLES:   # must not reach the program
                os.environ[name] = "C:/a/real/toolchain"
            with tempfile.TemporaryDirectory() as parent:
                verdict, text, lines, _took = start("closed", sandbox_parent=parent)
                self.assertEqual(verdict, "pass", (text, lines))
                self.assertEqual(os.listdir(parent), [], "the folders of the start are removed after it")
        finally:
            os.environ.clear()
            os.environ.update(saved)

    def test_a_program_that_installs_a_pack_or_moves_a_pointer_fails(self):
        for part, name in (("installs", "cmake.exe"), ("points", "latest")):
            with tempfile.TemporaryDirectory() as parent:
                verdict, text, _lines, took = start(part, sandbox_parent=parent)
                self.assertEqual(verdict, "fail", (part, text))
                self.assertIn("wrote a toolchain pack or pointer", text)
                self.assertIn(name, text)
                self.assertLess(took, 20)
                self.assertEqual(os.listdir(parent), [])

    def test_a_program_that_links_a_toolchain_into_the_package_fails(self):
        with tempfile.TemporaryDirectory() as package:
            verdict, text, _lines, _took = start("links", cwd=package)
            self.assertEqual(verdict, "fail", text)
            self.assertIn("a toolchain folder in the package", text)

    def test_a_package_whose_stamp_points_outside_it_is_not_started(self):
        with tempfile.TemporaryDirectory() as package, tempfile.TemporaryDirectory() as elsewhere:
            Path(package, "toolchain").mkdir()
            Path(package, "toolchain", ".psxrecomp-bin").write_text(elsewhere + "\n", encoding="utf-8")
            verdict, text, _lines, _took = start("marks", cwd=package)
            self.assertEqual(verdict, "fail", text)
            self.assertIn("points outside it", text)
            self.assertFalse(Path(package, "started.txt").exists(), "the program must not be started")
            # A stamp that names a folder inside the package is the package's own pack: the start is made.
            Path(package, "toolchain", "bin").mkdir()
            Path(package, "toolchain", ".psxrecomp-bin").write_text(
                str(Path(package, "toolchain", "bin")) + "\n", encoding="utf-8")
            verdict, text, _lines, _took = start("marks", cwd=package)
            self.assertEqual(verdict, "pass", text)
            self.assertTrue(Path(package, "started.txt").exists())

    def test_a_file_that_cannot_be_started_is_said_not_passed(self):
        verdict, text, _lines = tool.plain_start([str(ROOT / "LICENSE")], str(ROOT), timeout=5.0)
        self.assertEqual(verdict, "cannot-run")
        self.assertEqual(tool.main(["--exe", str(ROOT / "LICENSE")]), tool.CANNOT_RUN)
        self.assertEqual(tool.main(["--exe", str(ROOT / "no-such-setup-program")]), 1)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--play":
        sys.exit(play(sys.argv[2]))
    unittest.main()
