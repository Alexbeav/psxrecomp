"""The check that a test suite leaves the source tree alone must itself catch a dirty tree.

tools/tests/source_tree_clean.py runs as the first and the last test of a suite.
A check that cannot fail looks the same as a clean tree, so this proves each
kind of difference fails it, that an unchanged tree passes it with its counts,
and that it steps aside (NOT CHECKED, exit 0) where it cannot judge.

Hermetic: a made-up git checkout in a temporary folder. Skipped without git.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parent / "source_tree_clean.py"
GIT = shutil.which("git")


def put(path, text="text"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


@unittest.skipUnless(GIT, "git is not on PATH")
class SourceTreeClean(unittest.TestCase):
    def setUp(self):
        # resolved: the ceiling below must be the folder's real, long name
        self.tmp = Path(tempfile.mkdtemp(prefix="source-tree-clean-")).resolve()
        self.addCleanup(shutil.rmtree, str(self.tmp), ignore_errors=True)
        self.repo = self.tmp / "checkout"
        self.repo.mkdir()
        self.state = self.tmp / "state" / "snapshot.json"
        # git must not find a checkout above the temporary folder
        self.env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(self.tmp.parent))
        subprocess.run([GIT, "init", "-q", str(self.repo)], check=True, env=self.env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def tool(self, command, root=None, state=None):
        done = subprocess.run(
            [sys.executable, str(SCRIPT), command, "--root", str(root or self.repo),
             "--state", str(state or self.state)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=self.env,
            text=True, encoding="utf-8", errors="replace")
        return done.returncode, done.stdout

    def test_an_unchanged_tree_passes_and_says_what_it_counted(self):
        put(self.repo / "already-here.txt", "left by the developer")
        code, said = self.tool("snapshot")
        self.assertEqual(code, 0, said)
        self.assertIn("source tree snapshot: 1 entries", said)
        code, said = self.tool("compare")
        self.assertEqual(code, 0, said)
        self.assertIn("source tree unchanged by the suite: 1 entries listed by git status before, 1 after", said)

    def test_a_new_file_fails_and_is_named(self):
        self.tool("snapshot")
        put(self.repo / "licenses" / "framework" / "LICENSE", "written by a test")
        code, said = self.tool("compare")
        self.assertEqual(code, 1, said)
        self.assertIn("source tree CHANGED by the suite: 1 differences", said)
        self.assertIn("new      licenses/framework/LICENSE", said)

    def test_a_changed_file_fails(self):
        put(self.repo / "notes.txt", "before")
        self.tool("snapshot")
        put(self.repo / "notes.txt", "after")
        code, said = self.tool("compare")
        self.assertEqual(code, 1, said)
        self.assertIn("changed  notes.txt", said)

    def test_a_removed_file_fails(self):
        put(self.repo / "notes.txt", "before")
        self.tool("snapshot")
        (self.repo / "notes.txt").unlink()
        code, said = self.tool("compare")
        self.assertEqual(code, 1, said)
        self.assertIn("gone     notes.txt", said)

    def test_only_the_watched_folder_counts(self):
        watched = self.repo / "framework"
        watched.mkdir()
        self.tool("snapshot", root=watched)
        put(self.repo / "beside.txt", "outside the watched folder")
        code, said = self.tool("compare", root=watched)
        self.assertEqual(code, 0, said)
        self.assertIn("0 entries listed by git status before, 0 after", said)
        self.tool("snapshot", root=watched)
        put(watched / "inside.txt", "inside it")
        code, said = self.tool("compare", root=watched)
        self.assertEqual(code, 1, said)
        self.assertIn("new      framework/inside.txt", said)

    def test_a_snapshot_serves_one_comparison(self):
        self.tool("snapshot")
        self.assertEqual(self.tool("compare")[0], 0)
        self.assertFalse(self.state.exists())
        put(self.repo / "later.txt", "after the comparison")
        code, said = self.tool("compare")
        self.assertEqual(code, 0, said)
        self.assertIn("NOT CHECKED: no snapshot from this run", said)

    def test_outside_a_checkout_it_steps_aside(self):
        plain = self.tmp / "package"
        plain.mkdir()
        code, said = self.tool("snapshot", root=plain)
        self.assertEqual(code, 0, said)
        self.assertIn("NOT CHECKED", said)
        put(plain / "new.txt")
        code, said = self.tool("compare", root=plain)
        self.assertEqual(code, 0, said)
        self.assertIn("NOT CHECKED: the snapshot could not list the tree", said)


if __name__ == "__main__":
    unittest.main()
