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
import contextlib
import io
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

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
    if part == "holds-pid":
        Path("setup.pid").write_text(str(os.getpid()), encoding="utf-8")
        time.sleep(60)
        return 0
    return 2


def start(part, timeout=30.0, cwd=None, sandbox_parent=None):
    began = time.monotonic()
    verdict, text, lines = tool.plain_start([sys.executable, str(Path(__file__).resolve()), "--play", part],
                                            str(cwd or ROOT), timeout=timeout, sandbox_parent=sandbox_parent)
    return verdict, text, lines, time.monotonic() - began


def process_alive(pid):
    if os.name == "nt":
        import ctypes
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        api.OpenProcess.restype = ctypes.c_void_p
        api.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        api.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = api.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            if ctypes.get_last_error() == 87:           # no such PID
                return False
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            result = api.WaitForSingleObject(handle, 0)
            if result not in (0, 258):                  # signaled, WAIT_TIMEOUT
                raise ctypes.WinError(ctypes.get_last_error())
            return result == 258
        finally:
            api.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def setup_pid(folder):
    marker = Path(folder, "setup.pid")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if marker.is_file():
            value = marker.read_text(encoding="utf-8")
            if value:
                return int(value)
        time.sleep(0.02)
    raise AssertionError("the stand-in setup program did not start")


def wait_for_stop(pid):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and process_alive(pid):
        time.sleep(0.02)
    return not process_alive(pid)


def clean_stand_in(pid):
    # Exact-parent negative controls also run this test. Reap only this test's
    # recorded setup PID if the old tool leaves it running.
    if process_alive(pid):
        os.kill(pid, signal.SIGTERM)
        if not wait_for_stop(pid) and os.name != "nt":
            os.kill(pid, signal.SIGKILL)


class PlainStart(unittest.TestCase):
    def test_an_unwritable_log_keeps_the_verdict_and_fails_without_a_traceback(self):
        for verdict, label in (("pass", "PASS"), ("fail", "FAIL"), ("cannot-run", "NOT CHECKED")):
            with self.subTest(verdict=verdict), tempfile.TemporaryDirectory() as tmp:
                printed = io.StringIO()
                with mock.patch.object(tool, "plain_start", return_value=(verdict, "reason", ["program line"])), \
                        contextlib.redirect_stdout(printed):
                    code = tool.main(["--exe", str(ROOT / "LICENSE"), "--log", tmp])
                self.assertEqual(code, 1)
                shown = printed.getvalue()
                self.assertIn(label, shown.splitlines()[0])
                self.assertIn("  | program line", shown)
                self.assertIn("ERROR: cannot write stderr log", shown)
                self.assertNotIn("Traceback", shown)

    def test_main_writes_all_stderr_to_the_requested_log(self):
        lines = ["program line %d: \u03bb" % i for i in range(25)]
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp, "start.log")
            printed = io.StringIO()
            with mock.patch.object(tool, "plain_start", return_value=("pass", "reason", lines)), \
                    contextlib.redirect_stdout(printed):
                code = tool.main(["--exe", str(ROOT / "LICENSE"), "--log", str(log)])
            self.assertEqual(code, 0)
            self.assertEqual(log.read_bytes(), ("\n".join(lines) + "\n").encode("utf-8"))
            self.assertNotIn("  | " + lines[0], printed.getvalue())
            self.assertIn("  | " + lines[-1], printed.getvalue())

    def test_a_killed_tool_stops_its_setup_program(self):
        with tempfile.TemporaryDirectory() as tmp:
            wrapper = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--hold-tool", tmp],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL,
                                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            pid = None
            try:
                pid = setup_pid(tmp)
                self.assertTrue(process_alive(pid), "the stand-in must be alive before the tool is killed")
                wrapper.kill()
                wrapper.wait(timeout=10)
                self.assertTrue(wait_for_stop(pid), "killing the tool left its setup program running")
            finally:
                if wrapper.poll() is None:
                    wrapper.kill()
                wrapper.wait(timeout=10)
                if pid is not None:
                    clean_stand_in(pid)

    def test_an_interrupted_tool_stops_its_setup_program_and_cleans_its_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            pids = []

            def interrupt(*_args, **_kwargs):
                pids.append(setup_pid(tmp))
                raise KeyboardInterrupt

            try:
                with mock.patch.object(tool.threading.Event, "wait", side_effect=interrupt):
                    with self.assertRaises(KeyboardInterrupt):
                        start("holds-pid", cwd=tmp, sandbox_parent=tmp)
                self.assertTrue(pids)
                self.assertTrue(wait_for_stop(pids[0]), "interruption left the setup program running")
                self.assertEqual(list(Path(tmp).glob("plain-start-*")), [])
            finally:
                for pid in pids:
                    clean_stand_in(pid)

    def test_a_program_that_reaches_its_launcher_passes_and_is_stopped_at_once(self):
        os.environ["PSX_HEADLESS"] = "1"    # must not leak into the start
        try:
            verdict, text, lines, took = start("starts")
        finally:
            del os.environ["PSX_HEADLESS"]
        self.assertEqual(verdict, "pass", (text, lines))
        self.assertIn("reached host:before_run_window", text)
        self.assertLess(took, 20, "the program must be stopped at the stamp, not waited for")

    def test_what_the_program_wrote_is_shown_on_a_pass_too_and_goes_to_the_log(self):
        # A package with its config taken away passed on Pegasus and nothing said why: the tool
        # printed the program's lines only on a fail (PS1B-411).
        verdict, text, lines, _took = start("starts")
        self.assertEqual(verdict, "pass", (text, lines))
        printed = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "start.log"
            with contextlib.redirect_stdout(printed):
                code = tool.report(verdict, text, lines, "setup-program", str(log))
            logged = log.read_text(encoding="utf-8")
        self.assertEqual(code, 0)
        shown = printed.getvalue()
        self.assertIn("setup host plain start: PASS", shown.splitlines()[0])
        self.assertIn("  | psxrecomp: main() entered", shown)
        self.assertIn("host:before_run_window", shown.split("\n", 1)[1])
        self.assertIn("psxrecomp: main() entered\n", logged)
        self.assertIn(STAMP_LINE, logged)

    def test_a_fail_shows_the_program_lines_and_returns_one(self):
        verdict, text, lines, _took = start("refuses")
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            code = tool.report(verdict, text, lines, "setup-program")
        self.assertEqual(code, 1)
        self.assertIn("setup host plain start: FAIL", printed.getvalue().splitlines()[0])
        self.assertIn('key "name" not found', printed.getvalue())

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
    if len(sys.argv) == 3 and sys.argv[1] == "--hold-tool":
        tool.plain_start([sys.executable, str(Path(__file__).resolve()), "--play", "holds-pid"],
                         sys.argv[2], timeout=60, sandbox_parent=sys.argv[2])
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == "--play":
        sys.exit(play(sys.argv[2]))
    unittest.main()
