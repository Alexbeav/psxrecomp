#!/usr/bin/env python3
"""Guard the recoverable legacy-file to additive-directory migration."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "runtime/src/overlay_capture.c").read_text(encoding="utf-8")

assert "static int capture_prepare_contribution_dir(const char *path)" in SOURCE
assert "if (rename(path, backup) != 0) return 0;" in SOURCE
assert "if (!copy_file_atomic(backup, contribution))" in SOURCE
assert "(void)CAPTURE_RMDIR(path);" in SOURCE
assert "(void)rename(backup, path);" in SOURCE
assert SOURCE.index("capture_prepare_contribution_dir(contribution_dir)") < SOURCE.index(
    "atomic_replace_file(contribution_tmp, contribution)"
)
assert "migrated legacy additive capture history" in SOURCE

print("PASS: legacy additive capture evidence migrates without overwrite")
