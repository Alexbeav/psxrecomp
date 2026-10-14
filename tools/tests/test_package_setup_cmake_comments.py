#!/usr/bin/env python3
"""Run the packager's CMake admission gates on authored staged trees."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_package_setup_program_set import find_bash, shell_env, to_shell_path

PACKAGER = Path(__file__).resolve().parents[1] / "package_setup_host.sh"


class CMakeAdmission(unittest.TestCase):
    def test_comments_do_not_admit_or_require_paths(self):
        bash = find_bash()
        self.assertIsNotNone(bash, "bash is required to run the actual gate")
        text = PACKAGER.read_text(encoding="utf-8")
        gate = text.split("# --- Gate: the staged tree must be able to configure itself", 1)[1]
        gate = gate.split("# Second gate:", 1)[0]
        # The first line belongs to the comment banner, not the shell block.
        gate = gate.split("\n", 1)[1]
        for folder in ("", "program"):
            for kind in ("comment", "live", "commented_guard", "optional"):
                with self.subTest(folder=folder, kind=kind), tempfile.TemporaryDirectory() as tmp:
                    stage = Path(tmp)
                    root = stage / folder
                    root.mkdir(exist_ok=True)
                    ref = '${CMAKE_CURRENT_SOURCE_DIR}/missing.tga'
                    cmake = {
                        "comment": '  \t# LAUNCHER_BOXART "' + ref + '"\n',
                        "live": 'set(BOXART "' + ref + '")\n',
                        "commented_guard": '# if(EXISTS "' + ref + '")\nset(BOXART "' + ref + '")\n',
                        "optional": 'if(EXISTS "' + ref + '")\nset(BOXART "' + ref + '")\nendif()\n',
                    }[kind]
                    (root / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
                    script = stage / "gate.sh"
                    with script.open("w", encoding="utf-8", newline="\n") as out:
                        out.write('set -euo pipefail\nSTAGE="$1"\nSET_FOLDERS=()\n')
                        if folder:
                            out.write('SET_FOLDERS=(program)\n')
                        out.write(gate)
                    done = subprocess.run(
                        [bash, to_shell_path(script, bash), to_shell_path(stage, bash)],
                        env=shell_env(bash), capture_output=True, text=True,
                        encoding="utf-8", errors="replace")
                    self.assertEqual(done.returncode, 0 if kind in ("comment", "optional") else 1,
                                     done.stdout + done.stderr)
                    if done.returncode:
                        self.assertIn("missing.tga", done.stderr)


if __name__ == "__main__":
    unittest.main()
