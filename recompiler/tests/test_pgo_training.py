"""Build-time PGO boot overrides, the early BIOS check and the named failure cause."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
import psxrecomp_cli as cli


class PgoTrainingTests(unittest.TestCase):
    def train(self, root, spawn, *, train_runs=1):
        config = root / "game.toml"
        config.write_text("[runtime]\nopenbios = false\n", encoding="utf-8")
        with patch.object(cli, "_resolve_runtime_exe", return_value=(root / "game.exe", None)), \
             patch.object(cli.subprocess, "Popen", side_effect=spawn):
            cli.run_pgo_train(root, root / "build", exe_basename="game", target="game",
                              disc=root / "disc.chd", config=config, train_secs=60,
                              train_runs=train_runs, mute_host_audio=True, hide_video=True,
                              progress=Mock())

    def test_training_skips_intro_over_inherited_environment(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            seen = {}
            def spawn(argv, **kw):
                seen.update(kw["env"])
                (root / "build/probe.gcda").write_bytes(b"authored fixture")
                return Mock(pid=42, returncode=0, poll=Mock(return_value=0),
                            wait=Mock(return_value=0))
            with patch.dict(os.environ, {"PSX_BIOS_HLE": "0", "PSX_BIOS_HLE_KEEP_INTRO": "1"}):
                self.train(root, spawn)
            self.assertEqual(seen["PSX_BIOS_HLE"], "1")
            self.assertEqual(seen["PSX_BIOS_HLE_KEEP_INTRO"], "0")
            self.assertEqual(seen["PSX_HOST_MUTE"], "1")
            self.assertEqual(seen["PSX_HEADLESS"], "1")
            self.assertEqual(seen["PSX_EXIT_AFTER_MS"], "60000")

    def test_runtime_refusal_keeps_logs_and_reports_exit(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def spawn(argv, **kw):
                if hasattr(kw["stderr"], "write"):
                    kw["stderr"].write(b"no_bios: title requires a retail BIOS\n")
                return Mock(pid=42, returncode=1, poll=Mock(return_value=1),
                            wait=Mock(return_value=1))
            with self.assertRaisesRegex(RuntimeError, "exited with code 1"):
                self.train(root, spawn)
            log = root / "build/pgo/train-1.stderr.log"
            self.assertIn("no_bios", log.read_text(encoding="utf-8"))
            self.assertFalse((root / "build/pgo/default.profdata").exists())

    def test_killed_run_is_reaped_and_rejected_before_next_run_or_merge(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            events = []
            proc = Mock(pid=42, returncode=None, poll=Mock(return_value=None))
            def wait(timeout):
                events.append(("wait", timeout))
                if not proc.kill.called:
                    raise subprocess.TimeoutExpired("authored runtime", timeout)
                proc.returncode = -9
                return -9
            proc.wait.side_effect = wait
            proc.kill.side_effect = lambda: events.append(("kill", None))
            def spawn(argv, **kw):
                (root / "build/pgo/stale.profraw").write_bytes(b"authored stale profile")
                return proc
            with patch.object(cli, "_debug_quit", return_value=False), \
                 patch.object(cli, "_soft_stop"), \
                 patch.object(cli.subprocess, "run") as merge, \
                 patch.object(cli.subprocess, "Popen", side_effect=spawn) as popen:
                with self.assertRaisesRegex(RuntimeError, "stopped \\(exit code -9\\)"):
                    self.train(root, popen, train_runs=2)
                self.assertEqual(popen.call_count, 1)
                merge.assert_not_called()
            self.assertEqual(events, [
                ("wait", 150), ("wait", 5), ("wait", 5),
                ("kill", None), ("wait", 5),
            ])
            self.assertFalse((root / "build/pgo/default.profdata").exists())


    def test_failure_line_names_the_runtime_refusal(self):
        # PS1B-514 part C: the cause is in the line, not only in the stderr log.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def spawn(argv, **kw):
                kw["stderr"].write(b"psxrecomp: no BIOS selected; exiting.\n"
                                   b"psxrecomp: start refused (no_bios)\n")
                return Mock(pid=42, returncode=1, poll=Mock(return_value=1),
                            wait=Mock(return_value=1))
            with self.assertRaises(RuntimeError) as caught:
                self.train(root, spawn)
            self.assertIn("exited with code 1: start refused (no_bios); read ", str(caught.exception))

    def test_failure_line_repeats_no_other_log_text(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def spawn(argv, **kw):
                kw["stderr"].write(b"secret.bin: cannot open the file\n")
                return Mock(pid=42, returncode=3, poll=Mock(return_value=3),
                            wait=Mock(return_value=3))
            with self.assertRaises(RuntimeError) as caught:
                self.train(root, spawn)
            self.assertNotIn("secret", str(caught.exception))
            self.assertIn("exited with code 3; read ", str(caught.exception))

    def test_failure_line_says_when_the_cli_stopped_the_run(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            proc = Mock(pid=42, returncode=None, poll=Mock(return_value=None))
            def wait(timeout):
                if timeout == 150:
                    raise subprocess.TimeoutExpired("authored runtime", timeout)
                proc.returncode = 15
                return 15
            proc.wait.side_effect = wait
            with patch.object(cli, "_debug_quit", return_value=False), \
                 patch.object(cli, "_soft_stop"), \
                 patch.object(cli.subprocess, "Popen", return_value=proc) as popen:
                with self.assertRaises(RuntimeError) as caught:
                    self.train(root, popen)
            self.assertIn("run 1 did not exit within 150 s; stopped (exit code 15); read ",
                          str(caught.exception))


class PgoBiosPreflightTests(unittest.TestCase):
    """PS1B-514 part A: a missing BIOS is found before the instrumented build."""

    def test_default_title_needs_no_check(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(cli.pgo_bios_preflight({}, Path(folder)), "")

    def test_retail_only_title_without_bios_cfg_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            reason = cli.pgo_bios_preflight({"openbios": False}, Path(folder))
            self.assertIn("start refused (no_bios)", reason)

    def test_bios_cfg_naming_a_missing_file_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            (build / "bios.cfg").write_text(str(build / "gone.bin") + "\n", encoding="utf-8")
            self.assertNotEqual(cli.pgo_bios_preflight({"openbios": False}, build), "")

    def test_bios_cfg_naming_a_file_passes_without_reading_it(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            (build / "bios.cfg").write_text("bios/any.bin\n", encoding="utf-8")
            (build / "bios").mkdir()
            (build / "bios" / "any.bin").write_bytes(b"not a real image")
            self.assertEqual(cli.pgo_bios_preflight({"openbios": False}, build), "")

    def test_settings_toml_bios_path_counts(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            (build / "x.bin").write_bytes(b"x")
            (build / "settings.toml").write_text('[bios]\npath = "x.bin"\n', encoding="utf-8")
            self.assertEqual(cli.pgo_bios_preflight({"openbios": False}, build), "")

    def test_refusal_stops_before_the_instrumented_build(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "game.toml").write_text("x", encoding="utf-8")
            (root / "disc.chd").write_bytes(b"d")
            args = argparse.Namespace(config=str(root / "game.toml"), project_root=str(root),
                build_dir="build", target="psx-runtime", exe_basename="psx-runtime",
                no_pgo=False, force_pgo=True, disc=str(root / "disc.chd"), cmake_extra=[],
                train_secs=60, train_runs=1)
            events = []
            progress = Mock()
            patches = dict(load_sections=lambda _: {"runtime": {"openbios": False}},
                activate_embedded_toolchain=lambda *a: True,
                clamp_future_mtimes=lambda *a, **kw: 0,
                product_lto_enabled=lambda *a: False,
                pgo_merge_tool_available=lambda *a: "llvm-profdata",
                _assert_configured=lambda *a, **kw: None,
                stage_overlay_toolchain_for_product=lambda *a: None,
                stage_notices_for_product=lambda *a, **kw: {},
                _cmake_configure=lambda *a, **kw: events.append(("configure", kw["pgo"])),
                _cmake_build=lambda *a: events.append(("build",)),
                run_pgo_train=lambda *a, **kw: events.append(("train",)),
                _resolve_runtime_exe=lambda *a: (root / "game.exe", None))
            with patch.multiple(cli, **patches):
                result = cli.cmd_pgo_train(args, progress)
            self.assertEqual(result, cli.EXIT_OK)
            self.assertEqual(events, [("configure", ""), ("build",)])
            self.assertIn("start refused (no_bios)", progress.result.call_args.kwargs["pgo_skipped"])


if __name__ == "__main__":
    unittest.main()
