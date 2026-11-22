#!/usr/bin/env python3
"""PS1B-238: a cached unit never runs natively over code the interpreter hands
to static code: declared kernel patch words, or game text with a static entry
whose ranges still match RAM.

Builds the real overlay loader with overlay_kernel_patch_tier_harness.c and the
authored pair fixture (no game, BIOS or disc input), publishes one unit with
code [0x10000, 0x10008) and runs six modes, each in a fresh process: a declared
kernel range inside the unit and a matching static continuation inside it (no
native run, four misses under kernel_patch or static_text), and four where the
unit runs natively (a kernel range where the unit ends, nothing declared, an
image with no matching static entry in the unit, a matching static entry where
the unit ends). The harness says which check failed.
"""
from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path

import test_overlay_pair_dedup_runtime as pair


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(cmd, text=True, encoding="utf-8", errors="replace",
                            capture_output=True)
    if check and result.returncode:
        raise AssertionError(f"exit {result.returncode}: {cmd}\n{result.stdout}\n{result.stderr}")
    return result


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
        failed = []
        for mode in ("cover", "apart", "none", "static", "stale", "beyond"):
            cache = root / f"cache-{mode}"
            unit = pair.publish(cache, "gcc", f"00010000_11111111{ext}", fixture,
                                pair.manifest([(0x80010000, 8)]))
            result = run([str(harness), str(cache), mode, str(unit)], check=False)
            if result.returncode or f"PASS mode={mode}" not in result.stdout:
                failed.append(mode)
                print(f"FAIL static code tier {mode}\n{result.stdout}{result.stderr}", end="")
            else:
                print(f"PASS static code tier {mode}")
        if failed:
            raise SystemExit(f"failed modes: {' '.join(failed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
