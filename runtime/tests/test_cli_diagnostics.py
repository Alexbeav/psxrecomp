"""Diagnostic mode, CLI half (docs/DIAGNOSTIC_MODE.md).

Setup ships normal mode and a diagnostic product built from the same generated
sources, so a player can switch without recompiling; --collect-diagnostics
packs the runtime's report files for a GitHub issue and nothing else.
"""
import importlib.util
from pathlib import Path
import argparse
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_diag_under_test", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)


def _write_cache(build_dir, extra):
    """Stand in for CMake: the CLI verifies CMakeCache.txt after configure (4b9c85846)."""
    Path(build_dir).mkdir(parents=True, exist_ok=True)
    lines = [e[2:].replace("=", ":BOOL=", 1) for e in extra if e.startswith("-D") and "=" in e]
    (Path(build_dir) / "CMakeCache.txt").write_text("\n".join(lines), encoding="utf-8")


class DiagnosticRebuildTests(unittest.TestCase):
    def test_rebuild_builds_normal_then_diagnostic_product(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "game.toml").write_text("[game]\nname = \"fixture\"\n", encoding="utf-8")
            calls = []

            def fake_configure(project_root, build_dir, *, pgo, extra, progress):
                calls.append(("configure", Path(build_dir).name, [e for e in extra if "PSX_DEBUG_TOOLS" in e]))
                _write_cache(build_dir, extra)

            def fake_build(build_dir, target, progress):
                calls.append(("build", Path(build_dir).name))
                Path(build_dir).mkdir(parents=True, exist_ok=True)
                (Path(build_dir) / "Fixture.exe").write_bytes(b"MZ")

            args = argparse.Namespace(
                config=str(root / "game.toml"), project_root=str(root), build_dir="build-release",
                target="psx-runtime", exe_basename="Fixture", disc="", no_pgo=True, force_pgo=False,
                cmake_extra=[], diagnostic_dir="build-diagnostic", prune_after="")
            progress = Mock()
            with patch.object(cli, "activate_embedded_toolchain", lambda *a, **k: True), \
                 patch.object(cli, "stage_overlay_toolchain_for_product", lambda *a, **k: None), \
                 patch.object(cli, "_cmake_configure", side_effect=fake_configure), \
                 patch.object(cli, "_cmake_build", side_effect=fake_build), \
                 patch.object(cli, "_resolve_runtime_exe",
                              side_effect=lambda d, t, b: (Path(d) / "Fixture.exe", None)):
                code = cli.cmd_rebuild(args, progress)
            self.assertEqual(code, cli.EXIT_OK)
            # normal product first (debug tools off), diagnostic second (on)
            self.assertEqual(calls[0], ("configure", "build-release", ["-DPSX_DEBUG_TOOLS=OFF"]))
            self.assertEqual(calls[1], ("build", "build-release"))
            self.assertEqual(calls[2], ("configure", "build-diagnostic", ["-DPSX_DEBUG_TOOLS=ON"]))
            self.assertEqual(calls[3], ("build", "build-diagnostic"))
            result = progress.result.call_args.kwargs
            self.assertTrue(result["ok"])
            self.assertTrue(str(result["diagnostic_exe"]).endswith("Fixture.exe"))
            self.assertIn("build-diagnostic", str(result["diagnostic_exe"]))
            self.assertIsNone(result["diagnostic_error"])

    def test_diagnostic_build_failure_keeps_the_normal_product(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "game.toml").write_text("[game]\nname = \"fixture\"\n", encoding="utf-8")

            def fake_configure(project_root, build_dir, *, pgo, extra, progress):
                if Path(build_dir).name == "build-diagnostic":
                    raise RuntimeError("synthetic diagnostic configure failure")
                _write_cache(build_dir, extra)

            args = argparse.Namespace(
                config=str(root / "game.toml"), project_root=str(root), build_dir="build-release",
                target="psx-runtime", exe_basename="Fixture", disc="", no_pgo=True, force_pgo=False,
                cmake_extra=[], diagnostic_dir="build-diagnostic", prune_after="")
            progress = Mock()
            with patch.object(cli, "activate_embedded_toolchain", lambda *a, **k: True), \
                 patch.object(cli, "stage_overlay_toolchain_for_product", lambda *a, **k: None), \
                 patch.object(cli, "_cmake_configure", side_effect=fake_configure), \
                 patch.object(cli, "_cmake_build", lambda *a, **k: None), \
                 patch.object(cli, "_resolve_runtime_exe",
                              side_effect=lambda d, t, b: (Path(d) / "Fixture.exe", None)):
                code = cli.cmd_rebuild(args, progress)
            self.assertEqual(code, cli.EXIT_OK)
            result = progress.result.call_args.kwargs
            self.assertTrue(result["ok"])
            self.assertIsNone(result["diagnostic_exe"])
            self.assertIn("synthetic diagnostic configure failure", result["diagnostic_error"])


class DiagnosticsCollectorTests(unittest.TestCase):
    def test_collects_report_files_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rel = root / "build-release"; diag = root / "build-diagnostic"
            for d in (rel, diag, root / "saves", root / "disc", rel / "bios", diag / "bios"):
                d.mkdir(parents=True)
            (rel / "psx_last_run_report.json").write_text("{}", encoding="utf-8")
            (rel / "psx_game_version.txt").write_text("0.1.0\n", encoding="utf-8")
            (rel / "psxrecomp_exe_name-psx-runtime.txt").write_text("Fixture\n", encoding="utf-8")
            (diag / "psx_freeze_heartbeat.json").write_text("{}", encoding="utf-8")
            (diag / "psx_freeze_dump_0001.json").write_text("{}", encoding="utf-8")
            (diag / "psx_crash.txt").write_text("crash\n", encoding="utf-8")
            (diag / "settings.toml").write_text("[x]\n", encoding="utf-8")
            (root / "framework_pins.txt").write_text("psxrecomp=abc\n", encoding="utf-8")
            (root / "diagnostic-mode.txt").write_text("", encoding="utf-8")
            # never collected
            (root / "saves" / "card1.mcd").write_bytes(b"\x00" * 16)
            (rel / "bios" / "SCPH1001.BIN").write_bytes(b"\x01" * 16)
            (root / "disc" / "game.bin").write_bytes(b"\x02" * 16)
            (rel / "Fixture.exe").write_bytes(b"MZ")

            out = root / "out" / "diag.zip"
            summary = cli.collect_diagnostics(root, out)
            with zipfile.ZipFile(out) as z:
                names = set(z.namelist())
            self.assertEqual(names, {
                "build-release/psx_last_run_report.json",
                "build-release/psx_game_version.txt",
                "build-release/psxrecomp_exe_name-psx-runtime.txt",
                "build-diagnostic/psx_crash.txt",
                "build-diagnostic/psx_freeze_heartbeat.json",
                "build-diagnostic/psx_freeze_dump_0001.json",
                "framework_pins.txt",
                "diagnostic-mode.txt",
                "diagnostics-summary.json",
            })
            self.assertTrue(summary["diagnostic_mode_marker"])
            self.assertEqual(summary["build_dirs_present"]["build-diagnostic"], True)
            for forbidden in ("card1.mcd", "SCPH1001.BIN", "game.bin", "settings.toml", "Fixture.exe"):
                self.assertFalse(any(forbidden in n for n in names), forbidden)

    def test_cmd_diagnostics_defaults_to_a_stamped_zip_in_the_project(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "build-release").mkdir()
            (root / "build-release" / "psx_last_run_report.json").write_text("{}", encoding="utf-8")
            progress = Mock()
            args = argparse.Namespace(project_root=str(root), output="")
            self.assertEqual(cli.cmd_diagnostics(args, progress), cli.EXIT_OK)
            zips = list(root.glob("diagnostics-*.zip"))
            self.assertEqual(len(zips), 1)
            result = progress.result.call_args.kwargs
            self.assertTrue(result["ok"])
            self.assertEqual(result["files"], ["build-release/psx_last_run_report.json"])


if __name__ == "__main__":
    unittest.main()
