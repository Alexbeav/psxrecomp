#!/usr/bin/env python3
"""Drive the real loader with current and stale shards (overlay ABI v27).

The fork and upstream both exported "25" for different callback tables. v26 is
upstream's v25 layout plus the fork's slots. What the loader must do with each
kind of shard:

  current     built against this tree's header (v27): loads, runs natively, and
              reaches the host through upstream's v24/v25 slots and the fork's
              v26 slots;
  upstream    built against upstream's v25 header (the byte-for-byte fixture):
              refused, never called;
  g2          exports the tag a pin G2 shard exports (25): refused, never
              called.
  fork-v26    exports the former fork tag (26): refused, never called.

"Refused" is checked from the outside: the shard's own trace file has no line
(the loader called neither overlay_init nor a function), the harness exits 0,
the PC stays with the interpreter, the loader counts the rejection and keeps a
line that names it. Then the compiler's replacement takes the stale shard's
place and a rescan loads it, which is the regenerate path.

A cache folder left by another codegen version (cg12, what G2 wrote) is never
opened at all; the loader says whose cache it is.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import test_overlay_pair_dedup_runtime as base  # noqa: E402

RUNTIME = base.RUNTIME
TESTS = base.TESTS
EXT = ".dll" if platform.system() == "Windows" else ".so"
TWO = [(0x80010000, 4), (0x80010004, 4)]
SHARD_NAME = f"00010000_11111111{EXT}"


def compile_shard(gcc: str, out: pathlib.Path, *defines: str) -> None:
    command = [gcc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", "-shared",
               f"-I{RUNTIME / 'include'}", f"-I{TESTS}", *defines]
    if platform.system() != "Windows":
        command.append("-fPIC")
    command += [str(TESTS / "overlay_abi_gate_fixture.c"), "-o", str(out)]
    base.run(command)


def publish(cache: pathlib.Path, leaf: str, library: pathlib.Path) -> pathlib.Path:
    folder = cache / base.GAME / "gcc" / base.arch_abi() / leaf
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / SHARD_NAME
    shutil.copy2(library, target)
    target.with_suffix(".ranges").write_text(
        base.manifest(TWO), encoding="ascii", newline="")
    target.with_suffix(".resident").write_text(
        base.RESIDENT, encoding="ascii", newline="")
    return target


def run_harness(harness: pathlib.Path, cache: pathlib.Path, shard: pathlib.Path,
                mode: str, trace: pathlib.Path) -> str:
    env = dict(os.environ, PSX_PAIR_TEST_TRACE=str(trace))
    result = subprocess.run(
        [str(harness), str(cache), "abi-gate", str(shard), mode],
        text=True, encoding="utf-8", errors="replace", env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise AssertionError(
            f"abi-gate {mode}: harness exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result.stdout


def trace_lines(trace: pathlib.Path) -> list[str]:
    return trace.read_text(encoding="ascii").split() if trace.exists() else []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default=shutil.which("gcc") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.gcc:
        raise SystemExit("gcc/cc is required")

    sys.path.insert(0, str(base.ROOT / "tools"))
    import compile_overlays
    include = str(RUNTIME / "include")
    tag = compile_overlays.overlay_abi_tag(include, 0)
    codegen = compile_overlays.codegen_ver(include)
    assert tag == 27, f"this test describes ABI v27, the header says {tag}"

    with tempfile.TemporaryDirectory(
            prefix="psx-abi-gate-", ignore_cleanup_errors=True) as raw:
        tmp = pathlib.Path(raw)
        current = tmp / f"current{EXT}"
        upstream = tmp / f"upstream-v25{EXT}"
        g2 = tmp / f"g2-tag{EXT}"
        fork_v26 = tmp / f"fork-v26{EXT}"
        compile_shard(args.gcc, current)
        compile_shard(args.gcc, upstream, "-DTEST_UPSTREAM_V25=1")
        compile_shard(args.gcc, g2, "-DTEST_STALE_ABI=25")
        compile_shard(args.gcc, fork_v26, "-DTEST_STALE_ABI=26")
        harness = tmp / ("abi-harness" + (".exe" if EXT == ".dll" else ""))
        base.compile_harness(args.gcc, harness)
        leaf = base.codegen_leaf()
        assert leaf.startswith(f"cg{codegen}_"), leaf

        # 1. A shard of this build's ABI runs natively through every slot.
        cache = tmp / "cache-accept"
        shard = publish(cache, leaf, current)
        out = run_harness(harness, cache, shard, "accept", tmp / "t-accept")
        assert "PASS abi-gate accept" in out, out
        assert trace_lines(tmp / "t-accept") == ["init", "call"], out

        # 2. Preserve both v25 controls and reject the former fork's v26 tag.
        for name, library in (("upstream-v25", upstream), ("g2-tag", g2),
                              ("fork-v26", fork_v26)):
            cache = tmp / f"cache-{name}"
            shard = publish(cache, leaf, library)
            trace = tmp / f"t-{name}"
            out = run_harness(harness, cache, shard, "reject", trace)
            assert "PASS abi-gate reject" in out, out
            assert trace_lines(trace) == [], (
                f"{name}: the loader called into a refused shard: "
                f"{trace_lines(trace)}")
            assert "abi preflight: rejected 1 stale DLL(s), kept 0" in out, out
            assert f"ABI tag 0x{tag:X} only" in out, out
            assert shard.exists(), f"{name}: the loader deleted the stale shard"
            print(f"{name}: refused, not called. {out.splitlines()[0]}")

        # 3. Regenerate path: the compiler replaces the stale G2-tag shard and
        #    a rescan loads the new one.
        cache = tmp / "cache-replace"
        shard = publish(cache, leaf, g2)
        fresh = tmp / f"fresh{EXT}"
        shutil.copy2(current, fresh)
        trace = tmp / "t-replace"
        out = run_harness(harness, cache, shard, f"replace:{fresh}", trace)
        assert "PASS abi-gate replace" in out, out
        assert trace_lines(trace) == ["init", "call"], out
        print("stale shard replaced and loaded after rescan")

        # 4. With the sweep marker already present the scan-time sweep is
        #    skipped, so the per-load gate must refuse the shard by itself.
        cache = tmp / "cache-marker"
        shard = publish(cache, leaf, upstream)
        (shard.parent / f".abi_{tag:08x}.ok").write_bytes(b"")
        trace = tmp / "t-marker"
        out = run_harness(harness, cache, shard, "reject", trace)
        assert trace_lines(trace) == [], out
        assert "ABI/flavor mismatch" in out and "dll=0x19" in out, out
        print("per-load gate refuses a v25 shard behind a sweep marker")

        # 5. A cache folder from pin G2 (codegen 12) is never opened.
        cache = tmp / "cache-sibling"
        old_leaf = "cg12_6a0b6aaa_gc00000000_f0"
        assert old_leaf != leaf
        shard = publish(cache, old_leaf, g2)
        trace = tmp / "t-sibling"
        out = run_harness(harness, cache, shard, "sibling", trace)
        assert "PASS abi-gate sibling" in out, out
        assert trace_lines(trace) == [], out
        assert f"gcc/{old_leaf} is from codegen version 12" in out, out
        assert f"reads gcc/{leaf} (codegen version {codegen})" in out, out
        assert "abi_rejected=0" in out, out
        print(f"sibling cache ignored. {out.splitlines()[0]}")

    print("PASS: overlay ABI gate (v27 accepted; upstream v25, G2 and v26 tags "
          "refused; replacement loads; cg12 folder ignored)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
