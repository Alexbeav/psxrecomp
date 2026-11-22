#!/usr/bin/env python3
"""PS1B-238: a cached unit over declared kernel patch words never runs natively.

Builds the real overlay loader with overlay_kernel_patch_tier_harness.c and the
authored pair fixture (no game, BIOS or disc input), publishes one unit with
code [0x10000, 0x10008) and runs three modes, each in a fresh process: a
declared range inside the unit (no native run, four kernel_patch misses), a
range that starts where the unit ends, and no range (both run natively, so the
harness can see a native run). The harness says which check failed.
"""
from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path

import test_overlay_pair_dedup_runtime as pair


def run(cmd: list[str]) -> str:
    result = subprocess.run(cmd, text=True, encoding="utf-8", errors="replace",
                            capture_output=True)
    if result.returncode:
        raise AssertionError(f"exit {result.returncode}: {cmd}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("cc") or shutil.which("gcc"))
    args = parser.parse_args()
    if not args.cc:
        raise SystemExit("C compiler required")
    ext = ".dll" if platform.system() == "Windows" else ".so"
    with tempfile.TemporaryDirectory(prefix="psx-kernel-patch-tier-") as tmp:
        root = Path(tmp)
        fixture = root / f"fixture{ext}"
        pair.compile_fixture(args.cc, fixture, instance=1)
        harness = root / "kernel-patch-tier"
        cmd = [args.cc, "-std=c11", "-O0", "-Wall", "-Wextra",
               "-DPSX_NO_DEBUG_TOOLS", "-DPSX_OVERLAY_DLL_BUILD",
               "-DPSX_OVERLAY_TEST_CANDIDATE_CAP=4",
               f"-I{pair.RUNTIME / 'include'}"]
        if platform.system() != "Windows":
            cmd.append("-D_GNU_SOURCE")
        cmd += [str(pair.TESTS / "overlay_kernel_patch_tier_harness.c"),
                str(pair.RUNTIME / "src/overlay_loader.c"),
                str(pair.RUNTIME / "src/overlay_path_canon.c"),
                str(pair.RUNTIME / "src/overlay_posix.c"),
                str(pair.RUNTIME / "src/host_span_ring.c"),
                str(pair.RUNTIME / "src/interp_report.c"),
                str(pair.RUNTIME / "src/crc32.c"), "-o", str(harness)]
        if platform.system() != "Windows":
            cmd += ["-ldl", "-pthread"]
        run(cmd)
        for mode in ("cover", "apart", "none"):
            cache = root / f"cache-{mode}"
            unit = pair.publish(cache, "gcc", f"00010000_11111111{ext}", fixture,
                                pair.manifest([(0x80010000, 8)]))
            out = run([str(harness), str(cache), mode, str(unit)])
            assert f"PASS mode={mode}" in out, out
            print(f"PASS kernel patch tier {mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
