"""psxrecomp_cli.py rebuild: a build step that fails is built once more on its own (PS1B-342).

A Workbench build of Future Cop stopped because clang exited 1 with no
diagnostic under several concurrent builds; the same compile passed when re-run
by hand. In the parallel log the lines after a FAILED step can be other steps'
output, so a silent failure could not be told from a real one.

Each case configures a real two-file CMake project with Ninja and builds it
through cli._cmake_build. A compiler launcher stands for the fault:

  once     the compile of b.c exits 1 with no output the first time only:
           the build passes, and the log names the step that passed on its
           own and the free memory and compiler count at the failure.
  always   it exits 1 with no output every time: the build fails, and the
           error names the step, its exit code and "no diagnostic".
  error    b.c holds a real #error: the build fails, and the error carries the
           compiler's own line from the step built on its own.

python test_cli_build_failed_step.py [--cmake C] [--ninja N] [--cc CC]
Skips (exit 0, first line SKIP:) without cmake, ninja or a C compiler.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tools")]
import psxrecomp_cli as cli  # noqa: E402

LAUNCHER = r'''
import os, subprocess, sys
mode = os.environ.get("PS1B342_MODE", "")
state = os.environ.get("PS1B342_STATE", "")
if any(a.replace("\\", "/").endswith("/b.c") for a in sys.argv[1:]):
    if mode == "always" or (mode == "once" and not os.path.exists(state)):
        if mode == "once":
            open(state, "w").close()
        sys.exit(1)     # no output at all, like the clang of PS1B-342
sys.exit(subprocess.call(sys.argv[1:]))
'''

TOOLS: dict[str, str] = {}


class Log:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def log(self, line: str) -> None:
        self.lines.append(line)

    def text(self) -> str:
        return "\n".join(self.lines)


class FailedStepAloneTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get("TEMP"))
        self.root = Path(self.tmp.name)
        src = self.root / "src"
        src.mkdir()
        (src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(p342 C)\n"
            "add_library(p342 STATIC a.c b.c)\n", encoding="utf-8")
        (src / "a.c").write_text("int a(void) { return 1; }\n", encoding="utf-8")
        (src / "b.c").write_text("int b(void) { return 2; }\n", encoding="utf-8")
        launcher = self.root / "launcher.py"
        launcher.write_text(LAUNCHER, encoding="utf-8")
        self.build = self.root / "build"
        configure = subprocess.run(
            [TOOLS["cmake"], "-S", str(src), "-B", str(self.build), "-G", "Ninja",
             "-DCMAKE_MAKE_PROGRAM=" + TOOLS["ninja"], "-DCMAKE_C_COMPILER=" + TOOLS["cc"],
             "-DCMAKE_C_COMPILER_LAUNCHER=%s;-B;%s" % (Path(sys.executable).as_posix(), launcher.as_posix())],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(configure.returncode, 0, configure.stdout[-2000:] + configure.stderr[-2000:])
        self.env = dict(os.environ)
        os.environ["PS1B342_STATE"] = str(self.root / "failed-once")

    def tearDown(self) -> None:
        os.environ.clear()
        os.environ.update(self.env)
        self.tmp.cleanup()

    def run_build(self, mode: str) -> tuple[Log, str]:
        os.environ["PS1B342_MODE"] = mode
        log = Log()
        try:
            cli._cmake_build(self.build, "all", log)
        except RuntimeError as exc:
            return log, str(exc)
        return log, ""

    def test_once_silent_failure_passes_on_its_own(self) -> None:
        log, raised = self.run_build("once")
        self.assertEqual(raised, "", log.text()[-3000:])
        text = log.text()
        self.assertIn("passed on their own", text)
        self.assertRegex(text, r"b\.c\.obj on its own: exit 0|b\.c\.o on its own: exit 0")
        self.assertRegex(text, r"free memory (\d+\.\d GiB|unknown), (\d+|unknown) compiler or linker processes")
        self.assertTrue(any(p.name.startswith("libp342") or p.name == "p342.lib" for p in self.build.iterdir()),
                        sorted(p.name for p in self.build.iterdir()))

    def test_always_silent_failure_says_no_diagnostic(self) -> None:
        log, raised = self.run_build("always")
        self.assertRegex(raised, r"b\.c\.o(bj)? fails on its own \(exit 1\): no diagnostic", log.text()[-3000:])

    def test_real_error_carries_the_compiler_line(self) -> None:
        (self.root / "src" / "b.c").write_text("#error PS1B-342 real error\n", encoding="utf-8")
        log, raised = self.run_build("")
        self.assertRegex(raised, r"b\.c\.o(bj)? fails on its own \(exit 1\): .*PS1B-342 real error",
                         log.text()[-3000:])
        self.assertNotIn("passed on their own", log.text())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake")
    ap.add_argument("--ninja")
    ap.add_argument("--cc")
    args, rest = ap.parse_known_args()
    found = {"cmake": args.cmake or shutil.which("cmake"), "ninja": args.ninja or shutil.which("ninja"),
             "cc": args.cc or os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")}
    missing = [k for k, v in found.items() if not v or not Path(v).is_file()]
    if missing:
        print("SKIP: no %s for the real build (pass --cmake, --ninja, --cc)" % ", ".join(missing))
        return 0
    TOOLS.update({k: str(v) for k, v in found.items()})
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False, verbosity=2).result
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
