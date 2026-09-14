"""CLI half of the BIOS setup contract (registered by PR #343)."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_under_test", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)


class RetailBiosProfileTests(unittest.TestCase):
    def test_pairs_require_matching_descriptor(self):
        with tempfile.TemporaryDirectory() as temporary:
            fw = Path(temporary)
            generated = fw / "generated"
            generated.mkdir()
            for stem in ("OpenBIOS", "SCPH1001", "SCPH5552"):
                with self.subTest(stem=stem):
                    dispatch = generated / f"{stem}_dispatch.c"
                    full = generated / f"{stem}_full.c"
                    self.assertFalse(cli.bios_backend_present(fw, stem))
                    dispatch.write_text(f"const PsxBiosBackend {stem}_psx_bios_backend;", encoding="utf-8")
                    self.assertFalse(cli.bios_backend_present(fw, stem))
                    full.write_text("", encoding="utf-8")
                    self.assertTrue(cli.bios_backend_present(fw, stem))
                    dispatch.write_text("const PsxBiosBackend unrelated_psx_bios_backend;", encoding="utf-8")
                    self.assertFalse(cli.bios_backend_present(fw, stem))

    def test_profile_and_utf8_are_forwarded_and_failures_propagate(self):
        with tempfile.TemporaryDirectory() as temporary:
            fw = Path(temporary)
            (fw / "bios").mkdir()
            (fw / "bios/SCPH5552.toml").write_text('[program]\nid="SCPH-5552"\n', encoding="utf-8")
            progress = Mock()
            with patch.object(cli, "framework_root", return_value=fw), \
                 patch.object(cli, "find_psxrecomp_bios", return_value=fw / "bios-tool"), \
                 patch.object(cli.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "BIOS → generated", "")
                cli.regen_bios_profile(fw, "bios/SCPH5552.toml", progress=progress)
                args, kwargs = run.call_args
                self.assertEqual(args[0][1:], ["--config", "bios/SCPH5552.toml"])
                self.assertEqual(kwargs["cwd"], str(fw))
                self.assertEqual(kwargs["encoding"], "utf-8")
                self.assertEqual(kwargs["errors"], "replace")
                run.return_value = subprocess.CompletedProcess([], 2, "", "wrong image")
                with self.assertRaisesRegex(RuntimeError, "SCPH5552.*exit 2"):
                    cli.regen_bios_profile(fw, "bios/SCPH5552.toml", progress=progress)

    def test_retail_stem_resolution_prefers_explicit_then_profile_then_default(self):
        self.assertEqual(cli.retail_bios_stem({}, ""), "SCPH1001")
        self.assertEqual(cli.retail_bios_stem({"bios_config": "bios/SCPH5552.toml"}, ""), "SCPH5552")
        self.assertEqual(
            cli.retail_bios_stem({"bios_config": "L:\\private\\framework\\bios\\SCPH5552.toml"}, ""),
            "SCPH5552")
        self.assertEqual(cli.retail_bios_stem({"bios_config": "bios/SCPH1001.toml"}, "SCPH5552"), "SCPH5552")

    def test_stage_retail_bios_follows_the_pinned_stem_not_scph1001(self):
        """Regression for the wave-3 first-run loop: a title whose host links
        SCPH5552 must get an SCPH5552 backend pair from the player's dump,
        staged where bios/SCPH5552.toml actually loads it, never SCPH1001."""
        with tempfile.TemporaryDirectory() as temporary:
            fw = Path(temporary) / "psxrecomp"
            (fw / "bios").mkdir(parents=True)
            (fw / "bios/SCPH5552.toml").write_text(
                '[program]\nid = "SCPH-5552"\nrom = "bios/EUR-PSX-SCPH5552.bin"\n', encoding="utf-8")
            dump = Path(temporary) / "my-pal-bios.bin"
            dump.write_bytes(b"\x01" * 16)
            progress = Mock()

            def fake_regen(project_root, profile_rel, *, progress):
                stem = Path(profile_rel).stem
                gen = fw / "generated"
                gen.mkdir(exist_ok=True)
                (gen / f"{stem}_dispatch.c").write_text(
                    f"const PsxBiosBackend {stem}_psx_bios_backend;", encoding="utf-8")
                (gen / f"{stem}_full.c").write_text("", encoding="utf-8")

            with patch.object(cli, "regen_bios_profile", side_effect=fake_regen) as regen:
                dest = cli.stage_retail_bios(fw.parent, fw, dump, "SCPH5552", force=False, progress=progress)
                self.assertEqual(dest, fw / "bios/EUR-PSX-SCPH5552.bin")
                self.assertEqual(dest.read_bytes(), b"\x01" * 16)
                self.assertEqual(regen.call_count, 1)
                self.assertEqual(regen.call_args.args[1], "bios/SCPH5552.toml")
                self.assertTrue(cli.bios_backend_present(fw, "SCPH5552"))
                self.assertFalse(cli.bios_backend_present(fw, "SCPH1001"))
                cli.stage_retail_bios(fw.parent, fw, dump, "SCPH5552", force=False, progress=progress)
                self.assertEqual(regen.call_count, 1)
                cli.stage_retail_bios(fw.parent, fw, dump, "SCPH5552", force=True, progress=progress)
                self.assertEqual(regen.call_count, 2)
            with self.assertRaisesRegex(FileNotFoundError, "SCPH101"):
                cli.stage_retail_bios(fw.parent, fw, dump, "SCPH101", force=False, progress=progress)


if __name__ == "__main__":
    unittest.main()
