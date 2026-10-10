#!/usr/bin/env python3
"""Execute the real loader against authored DLLs; no retail or BIOS input."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

import test_overlay_pair_dedup_runtime as pair


def run(cmd, env=None):
    result = subprocess.run(cmd, text=True, encoding="utf-8", errors="replace",
                            capture_output=True, env=env)
    if result.returncode:
        raise AssertionError(f"exit {result.returncode}: {cmd}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("cc") or shutil.which("gcc"))
    args = parser.parse_args()
    if not args.cc:
        raise SystemExit("C compiler required")
    with tempfile.TemporaryDirectory(prefix="psx-tier-witness-") as tmp:
        root = Path(tmp)
        ext = ".dll" if platform.system() == "Windows" else ".so"
        fixture = root / f"fixture{ext}"
        pair.compile_fixture(args.cc, fixture, instance=1)
        for opt in ("-O0", "-O2"):
            for mode in ("normal", "prior-off", "selfcheck-end", "selfcheck-begin", "violation", "overflow"):
                harness = root / f"tier-{opt[1:]}-{mode}"
                cmd = [args.cc, "-std=c11", opt, "-Wall", "-Wextra",
                       "-DPSX_NO_DEBUG_TOOLS", "-DPSX_OVERLAY_DLL_BUILD",
                       "-DPSX_OVERLAY_TEST_CANDIDATE_CAP=4",
                       '-DPSX_FRAMEWORK_PIN="authored-source"',
                       '-DPSX_FRAMEWORK_TREE="authored-tree"',
                       f"-I{pair.RUNTIME / 'include'}"]
                if mode == "overflow":
                    cmd += ["-DPSX_OVERLAY_TEST_TIER_HOLDER_MAX=2",
                            "-DPSX_OVERLAY_TEST_TIER_COUNTER_MAX=3"]
                if platform.system() != "Windows":
                    cmd.append("-D_GNU_SOURCE")
                cmd += [str(pair.TESTS / "overlay_tier_witness_harness.c"),
                        str(pair.RUNTIME / "src/overlay_path_canon.c"),
                        str(pair.RUNTIME / "src/overlay_posix.c"),
                        str(pair.RUNTIME / "src/crc32.c"), "-o", str(harness)]
                if platform.system() != "Windows":
                    cmd += ["-ldl", "-pthread"]
                run(cmd)
                cache = root / f"cache-{opt[1:]}-{mode}"
                first = pair.publish(cache, "gcc", f"00010000_11111111{ext}", fixture,
                                     pair.manifest([(0x80010000, 4), (0x80010004, 4)]))
                # Distinct manifest prevents pair dedup from hiding admission.
                second = pair.publish(cache, "gcc", f"00011000_22222222{ext}", fixture,
                                      pair.manifest([(0x80010000, 8), (0x80010004, 4)]),
                                      pending=True)
                for ident in ("authored.route:1", 'bad"identifier'):
                    env = dict(os.environ, PSX_TIER_WITNESS_ID=ident)
                    observed = run([str(harness), str(cache), mode, str(first), str(second)], env)
                    w = json.loads(observed.strip().splitlines()[-1])
                    assert w["schema"] == "overlay-tier-witness-v1"
                    assert w["source"] == "authored-source" and w["tree"] == "authored-tree"
                    assert w["measurement"] == (ident if ident[0] == 'a' else '')
                    assert w["holders"] == {"replay": 0, "netplay": 0}
                    assert w["violations"] == (3 if mode == "violation" else 0)
                    assert w["overflow"] == (1 if mode == "overflow" else 0)
                    assert w["begin"]["native_entries"] == w["end"]["native_entries"] == 0
                    # Restore fresh pending fixture for the next independent process.
                    second.unlink()
                    for suffix in (".ranges", ".resident"):
                        Path(str(second.with_suffix(suffix))).unlink()
                    pair.publish(cache, "gcc", second.name, fixture,
                                 pair.manifest([(0x80010000, 8), (0x80010004, 4)]), pending=True)
                print(f"PASS tier witness {opt} {mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
