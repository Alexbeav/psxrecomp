"""CLAUDE.md rule 3 for the T211 code paths: no raw fprintf(stderr, ...).

Failures the player must see go through launcher_warning(). The older stderr uses
elsewhere in main.cpp are legacy debt and deliberately not covered here.

  python -m unittest runtime.tests.test_t211_no_stderr      (from the repo root)
"""
import re
import unittest
from pathlib import Path

MAIN = Path(__file__).resolve().parents[1] / "src" / "main.cpp"


def between(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


class T211NoStderr(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = MAIN.read_text(encoding="utf-8")
        cls.blocks = {
            "state helpers": between(cls.src, "static std::string exe_stem_from_argv(",
                                     "static std::filesystem::path resolve_existing_runtime_path("),
            "save-dir block": between(cls.src, "T211: $PSXRECOMP_SAVE_DIR",
                                      "Apply writable-state isolation"),
        }

    def test_blocks_were_found_and_are_not_trivial(self):
        for name, block in self.blocks.items():
            self.assertGreater(len(block.splitlines()), 10, name)

    def test_no_raw_stderr_in_t211_code(self):
        for name, block in self.blocks.items():
            self.assertIsNone(re.search(r"fprintf\s*\(\s*(\w+\s*\?\s*)?stderr", block),
                              "%s writes to stderr directly" % name)

    def test_failures_reach_the_player(self):
        helpers = self.blocks["state helpers"]
        self.assertIn("launcher_warning(kStateWarningTitle", helpers)
        self.assertIn("report_state_migration_failures(failures)", helpers)
        self.assertIn("report_state_migration_failures(failures)", self.blocks["save-dir block"])


if __name__ == "__main__":
    unittest.main()
