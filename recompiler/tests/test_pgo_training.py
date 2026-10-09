"""Build-time PGO boot overrides and early runtime refusal evidence."""
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
    def train(self, root, spawn):
        config = root / "game.toml"
        config.write_text("[runtime]\nopenbios = false\n", encoding="utf-8")
        with patch.object(cli, "_resolve_runtime_exe", return_value=(root / "game.exe", None)), \
             patch.object(cli.subprocess, "Popen", side_effect=spawn):
            cli.run_pgo_train(root, root / "build", exe_basename="game", target="game",
                              disc=root / "disc.chd", config=config, train_secs=60,
                              train_runs=1, mute_host_audio=True, hide_video=True,
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


if __name__ == "__main__":
    unittest.main()