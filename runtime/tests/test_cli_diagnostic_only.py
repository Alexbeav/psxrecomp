"""Diagnostic request of a set-up kit: `rebuild --diagnostic-only` (docs/DIAGNOSTIC_MODE.md).

Setup builds the normal product, prunes its intermediates, and may have replaced
it with a PGO build. Asking for diagnostic mode later must build the diagnostic
product alone: no configure, compile, overlay staging or prune of --build-dir.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_diag_only_under_test", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)


def _args(root, **overrides):
    fields = dict(
        config=str(root / "game.toml"), project_root=str(root), build_dir="build-release",
        target="psx-runtime", exe_basename="Fixture", disc="", no_pgo=False, force_pgo=False,
        cmake_extra=[], diagnostic_dir="build-diagnostic", diagnostic_only=True, prune_after="")
    fields.update(overrides)
    return argparse.Namespace(**fields)


def _write_cache(build_dir, extra):
    """Stand in for CMake: the CLI checks CMakeCache.txt after configure."""
    Path(build_dir).mkdir(parents=True, exist_ok=True)
    lines = [e[2:].replace("=", ":BOOL=", 1) for e in extra if e.startswith("-D") and "=" in e]
    (Path(build_dir) / "CMakeCache.txt").write_text("\n".join(lines), encoding="utf-8")


def _snapshot(directory):
    return {p.relative_to(directory).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns)
            for p in sorted(Path(directory).rglob("*")) if p.is_file()}


class DiagnosticOnlyTests(unittest.TestCase):
    def _run(self, root, args, *, fail_configure=False):
        calls = []

        def fake_configure(project_root, build_dir, *, pgo, extra, progress):
            calls.append(("configure", Path(build_dir).name, pgo))
            if fail_configure:
                raise RuntimeError("synthetic diagnostic configure failure")
            _write_cache(build_dir, extra)

        def fake_build(build_dir, target, progress):
            calls.append(("build", Path(build_dir).name))
            (Path(build_dir) / "Fixture.exe").write_bytes(b"MZ-diagnostic")

        def fake_stage(project_root, exe_dir, progress):
            calls.append(("stage", Path(exe_dir).name))

        def fake_prune(project_root, build_dir, modes, progress):
            calls.append(("prune", Path(build_dir).name))

        def forbidden(*a, **k):
            calls.append(("normal-product-step",))
            raise AssertionError("diagnostic-only touched the normal product")

        progress = Mock()
        with patch.object(cli, "activate_embedded_toolchain", lambda *a, **k: True), \
             patch.object(cli, "_cmake_configure", side_effect=fake_configure), \
             patch.object(cli, "_cmake_build", side_effect=fake_build), \
             patch.object(cli, "stage_overlay_toolchain_for_product", side_effect=fake_stage), \
             patch.object(cli, "prune_after_rebuild", side_effect=fake_prune), \
             patch.object(cli, "_configure_product", side_effect=forbidden), \
             patch.object(cli, "run_pgo_train", side_effect=forbidden), \
             patch.object(cli, "_resolve_runtime_exe",
                          side_effect=lambda d, t, b: (Path(d) / "Fixture.exe", None)):
            code = cli.cmd_rebuild(args, progress)
        return code, calls, progress

    def _kit(self, temporary, toml):
        root = Path(temporary)
        (root / "game.toml").write_text(toml, encoding="utf-8")
        release = root / "build-release"
        release.mkdir()
        # What setup leaves behind: the product and assets, intermediates pruned.
        (release / "Fixture.exe").write_bytes(b"MZ-normal-pgo")
        (release / "assets").mkdir()
        (release / "assets" / "font.bin").write_bytes(b"\x01\x02")
        return root, release

    def test_builds_only_the_diagnostic_product(self):
        for toml in ('[game]\nname = "fixture"\n',
                     '[game]\nname = "fixture"\n\n[pgo]\nenabled = true\n'):
            with self.subTest(pgo_enabled="[pgo]" in toml), tempfile.TemporaryDirectory() as temporary:
                root, release = self._kit(temporary, toml)
                before = _snapshot(release)
                code, calls, progress = self._run(root, _args(root))
                self.assertEqual(code, cli.EXIT_OK)
                self.assertEqual(calls, [("configure", "build-diagnostic", ""),
                                         ("build", "build-diagnostic"),
                                         ("stage", "build-diagnostic")])
                self.assertEqual(_snapshot(release), before)
                result = progress.result.call_args.kwargs
                self.assertTrue(result["ok"])
                self.assertTrue(result["diagnostic_only"])
                self.assertIn("build-diagnostic", result["diagnostic_exe"])

    def test_failure_is_an_error_and_leaves_the_normal_product(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, release = self._kit(temporary, '[game]\nname = "fixture"\n')
            before = _snapshot(release)
            code, calls, progress = self._run(root, _args(root), fail_configure=True)
            self.assertEqual(code, cli.EXIT_ERROR)
            self.assertEqual(calls, [("configure", "build-diagnostic", "")])
            self.assertEqual(_snapshot(release), before)
            self.assertIn("synthetic diagnostic configure failure", progress.error.call_args.args[0])
            progress.result.assert_not_called()

    def test_usage_errors(self):
        cases = {
            "no diagnostic dir": dict(diagnostic_dir=""),
            "force pgo": dict(force_pgo=True),
            "prune": dict(prune_after="build-intermediates"),
            "same dir": dict(diagnostic_dir="build-release"),
        }
        for name, overrides in cases.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as temporary:
                root, release = self._kit(temporary, '[game]\nname = "fixture"\n')
                code, calls, progress = self._run(root, _args(root, **overrides))
                self.assertEqual(code, cli.EXIT_USAGE)
                self.assertEqual(calls, [])

    def test_mtime_clamp_skips_both_build_trees(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            future = time.time() + 3600
            paths = {name: root / name / "file.txt"
                     for name in ("build-release", "build-diagnostic", "generated")}
            for p in paths.values():
                p.parent.mkdir()
                p.write_text("x", encoding="utf-8")
                os.utime(p, (future, future))
            n = cli.clamp_future_mtimes(root, skip=[root / "build-release", root / "build-diagnostic"])
            self.assertEqual(n, 1)
            self.assertGreater(paths["build-release"].stat().st_mtime, time.time())
            self.assertGreater(paths["build-diagnostic"].stat().st_mtime, time.time())
            self.assertLessEqual(paths["generated"].stat().st_mtime, time.time())
            # A single Path still works for the two-product rebuild.
            os.utime(paths["generated"], (future, future))
            self.assertEqual(cli.clamp_future_mtimes(root, skip=root / "build-release"), 2)


if __name__ == "__main__":
    unittest.main()
