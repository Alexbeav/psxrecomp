#!/usr/bin/env python3
"""Keep tracked PowerShell scripts parseable by Windows PowerShell 5.1.

PowerShell 5.1 reads a BOM-less .ps1 as the ANSI code page, so a UTF-8 em
dash inside a double-quoted string decodes to bytes that include a curly
quote and terminate the string early. Scripts must be ASCII or carry a BOM.
"""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
UTF8_BOM = b"\xef\xbb\xbf"


def tracked_ps1_files() -> list[Path]:
    out = subprocess.check_output(
        ["git", "ls-files", "-z", "*.ps1"], cwd=REPO_ROOT
    )
    return [REPO_ROOT / name for name in out.decode().split("\0") if name]


class Ps1EncodingTests(unittest.TestCase):
    def test_non_ascii_ps1_files_have_utf8_bom(self) -> None:
        files = tracked_ps1_files()
        self.assertTrue(files, "no tracked .ps1 files found")
        offenders = []
        for path in files:
            data = path.read_bytes()
            if data.startswith(UTF8_BOM):
                continue
            for lineno, line in enumerate(data.split(b"\n"), 1):
                if any(byte > 0x7F for byte in line):
                    offenders.append(f"{path.relative_to(REPO_ROOT)}:{lineno}")
        self.assertEqual(
            offenders,
            [],
            "non-ASCII bytes in BOM-less .ps1 (PowerShell 5.1 misparses them)",
        )


if __name__ == "__main__":
    unittest.main()
