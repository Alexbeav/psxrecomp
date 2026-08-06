#!/usr/bin/env python3
"""Compile the real overlay serializers and exercise bounded JSON output."""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import tempfile

import test_overlay_pair_dedup_runtime as pair_test


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default=shutil.which("gcc") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.gcc:
        raise SystemExit("gcc/cc is required")

    with tempfile.TemporaryDirectory(
            prefix="psx-overlay-report-", ignore_cleanup_errors=True) as raw:
        harness = pathlib.Path(raw) / "overlay-report-bounds.exe"
        pair_test.compile_harness(
            args.gcc, harness, ("-DPSX_OVERLAY_TEST_HOOKS",))
        result = subprocess.run(
            [str(harness), "--report-bounds"], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise AssertionError(
                f"bounds harness failed ({result.returncode})\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")

    observed = {}
    for line in result.stdout.splitlines():
        name, cap_text, written_text, payload = line.split("\t", 3)
        cap = int(cap_text)
        written = int(written_text)
        observed[name, cap] = written
        if payload != "<empty>":
            parsed = json.loads(payload)
            assert isinstance(parsed, dict)

    assert observed["shadow", 128 * 1024] < 128 * 1024
    assert observed["shadow", 512 * 512 + 1024] > 128 * 1024
    assert observed["native", 2 * 1024 * 1024] > observed[
        "native", 512 * 512 + 1024]
    print("PASS: overlay crash serializers preserve bounds and valid JSON")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
