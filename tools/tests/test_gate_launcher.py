"""tools/ci/gate_launcher.py: the launcher pairing check and the ctest skip reader (PS1B-485).

Stdlib and git only. Each case runs the tool as the gate does, in a process of its own.
"""
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CI = Path(__file__).resolve().parents[1] / "ci"
TOOL = CI / "gate_launcher.py"

# ctest's own output for the two trees of trial run 18 (PS1B-485, comment 7-6289).
CTEST_WITHOUT = """\
1/7 Test #274: codegen_host_posix_toolchain_test .....   Passed    0.38 sec
2/7 Test #275: codegen_host_disc_cfg_handoff_test ....   Passed    0.14 sec
3/7 Test #276: codegen_host_handover_launcher_test ...***Skipped   0.05 sec
4/7 Test #277: toolchain_readonly_switch_test ........   Passed    2.48 sec

100% tests passed out of 4

The following tests did not run:
\t276 - codegen_host_handover_launcher_test (Skipped)
"""
CTEST_RECOMP_WITHOUT = """\
1/2 Test #147: codegen_host_bios_stems ..........   Passed    0.05 sec
2/2 Test #148: windows_rebuild_helper ...........***Skipped   0.15 sec
"""
CTEST_WITH = CTEST_WITHOUT.replace("***Skipped   0.05", "   Passed    1.59").split("\n\n")[0] + "\n"
MUST = ["codegen_host_handover_launcher_test", "windows_rebuild_helper"]


def run(*args):
    done = subprocess.run([sys.executable, "-B", str(TOOL), *map(str, args)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    return done.returncode, done.stdout.strip(), done.stderr.strip()


def git(root, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false",
                    "-C", str(root), *args], check=True, capture_output=True, text=True, encoding="utf-8",
                   errors="replace")


class PinFile(unittest.TestCase):
    def test_pin_file_names_one_full_commit(self):
        lines = [x.strip() for x in (CI / "recomp_ui_pin.txt").read_text(encoding="utf-8").splitlines()]
        ids = [x for x in lines if x and not x.startswith("#")]
        self.assertEqual(len(ids), 1)
        self.assertRegex(ids[0], r"^[0-9a-f]{40}$")


class SkipReader(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def log(self, name, text):
        path = Path(self.dir.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_names_each_test_once_across_both_trees(self):
        rt, rc = self.log("rt.log", CTEST_WITHOUT), self.log("rc.log", CTEST_RECOMP_WITHOUT)
        code, out, _ = run("skips", rt, rc)
        self.assertEqual((code, out), (0, "skipped: 2 (codegen_host_handover_launcher_test windows_rebuild_helper)"))

    def test_a_later_pass_does_not_hide_an_earlier_skip(self):
        # "Skipped wins" holds across logs too: the name is Skipped in the first tree, Passed in the second.
        first = self.log("rt.log", CTEST_RECOMP_WITHOUT)
        later = self.log("rc.log", "1/1 Test #9: windows_rebuild_helper ...........   Passed    1.00 sec\n")
        for order in ((first, later), (later, first)):
            code, out, _ = run("skips", *order, "--must-run", "windows_rebuild_helper")
            self.assertEqual(code, 1, order)
            self.assertEqual(out, "skipped: 1 (windows_rebuild_helper)", order)

    def test_every_git_call_leaves_the_index_alone(self):
        import importlib.util
        from unittest import mock
        spec = importlib.util.spec_from_file_location("gate_launcher_under_test", TOOL)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        done = subprocess.CompletedProcess([], 0, stdout="x\n", stderr="")
        with mock.patch.object(module.subprocess, "run", return_value=done) as called:
            module.git(Path("."), "status", "--porcelain")
        self.assertEqual(called.call_args.kwargs["env"]["GIT_OPTIONAL_LOCKS"], "0")

    def test_zero_when_nothing_was_left_out(self):
        code, out, _ = run("skips", self.log("rt.log", CTEST_WITH))
        self.assertEqual((code, out), (0, "skipped: 0 ()"))

    def test_must_run_refuses_a_test_that_did_not_pass(self):
        rt, rc = self.log("rt.log", CTEST_WITHOUT), self.log("rc.log", CTEST_RECOMP_WITHOUT)
        code, out, err = run("skips", rt, rc, "--must-run", *MUST)
        self.assertEqual(code, 1)
        self.assertIn("skipped: 2", out)
        for name in MUST:
            self.assertIn(name + " (Skipped)", err)

    def test_must_run_refuses_a_test_missing_from_the_logs(self):
        code, _, err = run("skips", self.log("rt.log", CTEST_WITH), "--must-run", "windows_rebuild_helper")
        self.assertEqual(code, 1)
        self.assertIn("windows_rebuild_helper (not in the logs)", err)

    def test_must_run_refuses_a_failed_test(self):
        text = CTEST_WITH.replace("   Passed    1.59", "***Failed    1.59")
        code, _, err = run("skips", self.log("rt.log", text), "--must-run", "codegen_host_handover_launcher_test")
        self.assertEqual(code, 1)
        self.assertIn("codegen_host_handover_launcher_test (Failed)", err)

    def test_must_run_accepts_tests_that_passed(self):
        rt = self.log("rt.log", CTEST_WITH)
        rc = self.log("rc.log", CTEST_RECOMP_WITHOUT.replace("***Skipped   0.15", "   Passed    1.51"))
        code, out, err = run("skips", rt, rc, "--must-run", *MUST)
        self.assertEqual((code, out, err), (0, "skipped: 0 ()", ""))


class LauncherCheck(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name) / "recomp-ui"
        (self.root / "src").mkdir(parents=True)
        (self.root / "src" / "recomp_launcher.h").write_text("/* launcher */\n", encoding="utf-8")
        git(self.root, "init", "-q")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "launcher")
        out = subprocess.run(["git", "-C", str(self.root), "rev-parse", "HEAD"], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", check=True).stdout.strip()
        self.head = out
        self.pin = Path(self.dir.name) / "pin.txt"
        self.pin.write_text("# comment\n" + self.head + "\n", encoding="utf-8")

    def test_the_paired_commit_prints_the_record_line(self):
        code, out, err = run("check", self.root, "--pin", self.pin)
        self.assertEqual((code, err), (0, ""))
        self.assertRegex(out, r"^recomp_ui_root=\S+ recomp_ui_head=%s recomp_ui_tree=[0-9a-f]{40}$" % self.head)

    def test_a_twelve_digit_pin_is_enough(self):
        self.pin.write_text(self.head[:12] + "\n", encoding="utf-8")
        self.assertEqual(run("check", self.root, "--pin", self.pin)[0], 0)

    def test_another_commit_is_refused_with_both_ids(self):
        (self.root / "src" / "recomp_launcher.h").write_text("/* newer */\n", encoding="utf-8")
        git(self.root, "commit", "-q", "-am", "newer")
        code, out, err = run("check", self.root, "--pin", self.pin)
        self.assertEqual((code, out), (2, ""))
        self.assertIn(self.head, err)
        self.assertRegex(err, r"is at [0-9a-f]{40}")

    def test_a_changed_file_is_refused(self):
        (self.root / "src" / "recomp_launcher.h").write_text("/* edited */\n", encoding="utf-8")
        code, _, err = run("check", self.root, "--pin", self.pin)
        self.assertEqual(code, 2)
        self.assertIn("not clean", err)

    def test_an_untracked_file_is_refused(self):
        (self.root / "stray.txt").write_text("x\n", encoding="utf-8")
        self.assertEqual(run("check", self.root, "--pin", self.pin)[0], 2)

    def test_a_folder_without_the_launcher_header_is_refused(self):
        empty = Path(self.dir.name) / "empty"
        empty.mkdir()
        code, _, err = run("check", empty, "--pin", self.pin)
        self.assertEqual(code, 2)
        self.assertIn("recomp_launcher.h", err)

    def test_a_pin_file_without_a_commit_is_refused(self):
        self.pin.write_text("# nothing\n", encoding="utf-8")
        code, _, err = run("check", self.root, "--pin", self.pin)
        self.assertEqual(code, 2)
        self.assertIn("no commit id", err)


if __name__ == "__main__":
    unittest.main()
