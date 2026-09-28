#!/usr/bin/env python3
"""Build the real overlay loader and prove shards run only for their segment.

Overlay shards are compiled for the KSEG0 address of their bytes and bake that
segment into every PC they produce: jal/jalr link values, jump and exception
resume PCs, I-cache fetch tags. OpenBIOS's exception path enters its RAM patch
slots at KUSEG (0x0000281C), and a KSEG0 shard run there wrote $ra=0x8000282C
instead of 0x0000282C and missed I-cache lines the kernel had just filled with
KUSEG tags: R4 warm (native) and cold (interpreted) overlay-cache runs took the
next VBlank IRQ at different instructions and split at guest frame 194.

This drives the loader harness (overlay_pair_dedup_harness.c) with a fixture
shard at 0x80010000: KUSEG and KSEG1 aliases must fall to the interpreter
without calling the shard, and the KSEG0 PC must still run native.

Usage: python runtime/tests/test_overlay_segment_gate.py
Exit 0 = PASS.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import test_overlay_pair_dedup_runtime as pair  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default=shutil.which("gcc") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.gcc:
        raise SystemExit("gcc/cc is required")
    ext = ".dll" if platform.system() == "Windows" else ".so"
    exe = ".exe" if platform.system() == "Windows" else ""
    with tempfile.TemporaryDirectory(
            prefix="psx-segment-gate-", ignore_cleanup_errors=True) as raw:
        tmp = pathlib.Path(raw)
        shard = tmp / f"shard{ext}"
        harness = tmp / f"segment-harness{exe}"
        pair.compile_fixture(args.gcc, shard, instance=1)
        pair.compile_harness(args.gcc, harness)
        cache = tmp / "cache"
        first = pair.publish(cache, "gcc", f"00010000_11111111{ext}", shard,
                             pair.manifest([(0x80010000, 4), (0x80010004, 4)]))
        os.environ.pop("PSX_PAIR_TEST_TRACE", None)
        pair.run([str(harness), str(cache), "segment-alias", str(first),
                  str(tmp / f"unused{ext}")])
    print("PASS: overlay shards run only for the segment they were compiled for")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
