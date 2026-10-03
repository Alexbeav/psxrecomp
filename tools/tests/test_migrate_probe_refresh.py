#!/usr/bin/env python3
"""Project Studio's probe_disc_refresh writes only disc identity.

probe_disc.py renders a complete scaffold game.toml and seed list. Migrating
an existing title used to write both straight over the project's own files,
discarding hand-tuned runtime/widescreen/overlay configuration and curated
seeds (found migrating TonyHawksProSkater2Recomp, 2026-10-01). These cases pin
the merge: existing configuration wins; [prepare_disc] and [netplay] are the
probe's; [game] only gains identity keys it lacks; seeds are a union.

The probe process itself is replaced by a fake that writes scripted output to
the paths on its command line, so no disc is needed.

Run:  python3 tools/tests/test_migrate_probe_refresh.py
"""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

FW = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(FW / "tools" / "new_project_layout"))

from project_studio import ops  # noqa: E402
from project_studio.models import MigrateOptions  # noqa: E402

EXISTING = """[game]
name = "Tony Hawk's Pro Skater 2"
id = "SLUS-01066"
exe = "thps2/SLUS_010.66"
# page-rounded on purpose
text_size = "0xD3000"

[runtime]
debug_port = 4530
openbios = true

[widescreen.cull]
plane_nx_sites = ["0x80091390", "0x80091880"]

[[widescreen.cull.keep]]
address = "0x8009518C"
expected = "0x020F402A"
result = 0

[audit]
[[audit.regions]]
name = "Text"
"""

PROBED = """# Autofilled by tools/new_project_layout/probe_disc.py from your legal dump.

[game]
name = "Tony Hawk's Pro Skater 2"
id = "SLUS-01066"
players = 2
exe = "disc/SLUS_010.66"
text_size = "0x000D2800"

# Digests are for data Track 01 (first BINARY FILE / TRACK 01).
[prepare_disc]
out_dir = "disc"
known_md5 = [
  "ac85ae22723eaf944b0f899aa53244dd",
]

[recompiler]
seeds = "seeds/ghidra_funcs.txt"

[runtime]
overlay_cache = true

# Online mount gate
[netplay]
require_cue = true
required_tracks = 1
"""

SEEDS_EXISTING = """# Seeds for Tony Hawk's Pro Skater 2 (SLUS-01066)
# curated
0x80010000
0x80091390
"""

SEEDS_PROBED = """# Auto-scanned JAL targets (+ entry) from SLUS_010.66
0x80010000
0x80010070
0x80091390
"""


class MergeGameTomlTest(unittest.TestCase):
    def test_existing_configuration_survives(self):
        merged = ops.merge_probe_game_toml(EXISTING, PROBED)
        for kept in ("debug_port = 4530", "openbios = true",
                     'plane_nx_sites = ["0x80091390", "0x80091880"]',
                     "[[widescreen.cull.keep]]", 'address = "0x8009518C"',
                     "[[audit.regions]]", "# page-rounded on purpose",
                     'exe = "thps2/SLUS_010.66"', 'text_size = "0xD3000"'):
            self.assertIn(kept, merged)
        # Sections the probe renders but does not own are never imported.
        self.assertNotIn("overlay_cache", merged)
        self.assertNotIn("[recompiler]", merged)
        self.assertNotIn('exe = "disc/SLUS_010.66"', merged)

    def test_game_gains_only_missing_identity_keys(self):
        merged = ops.merge_probe_game_toml(EXISTING, PROBED)
        game = merged.split("[prepare_disc]")[0]
        self.assertIn("players = 2", game)
        self.assertEqual(game.count("text_size"), 1)

    def test_identity_sections_inserted(self):
        merged = ops.merge_probe_game_toml(EXISTING, PROBED)
        self.assertLess(merged.index("[game]"), merged.index("[prepare_disc]"))
        self.assertLess(merged.index("[prepare_disc]"), merged.index("[runtime]"))
        self.assertIn("# Digests are for data Track 01", merged)
        self.assertTrue(merged.rstrip().endswith("required_tracks = 1"))
        self.assertEqual(merged.count("[netplay]"), 1)

    def test_identity_sections_replaced(self):
        stale = EXISTING + '\n[prepare_disc]\nknown_md5 = ["old"]\n\n[netplay]\nrequired_tracks = 9\n'
        merged = ops.merge_probe_game_toml(stale, PROBED)
        self.assertNotIn('"old"', merged)
        self.assertNotIn("required_tracks = 9", merged)
        self.assertEqual(merged.count("[prepare_disc]"), 1)
        self.assertEqual(merged.count("[netplay]"), 1)

    def test_idempotent(self):
        once = ops.merge_probe_game_toml(EXISTING, PROBED)
        self.assertEqual(ops.merge_probe_game_toml(once, PROBED), once)


class MergeSeedsTest(unittest.TestCase):
    def test_union_keeps_curated_file(self):
        text, added = ops.merge_seed_files(SEEDS_EXISTING, SEEDS_PROBED)
        self.assertEqual(added, 1)
        self.assertTrue(text.startswith(SEEDS_EXISTING))
        self.assertIn("0x80010070", text)
        again, added_again = ops.merge_seed_files(text, SEEDS_PROBED)
        self.assertEqual((again, added_again), (text, 0))


def fake_probe(cmd, cwd, dry_run):
    def arg(flag):
        return Path(cmd[cmd.index(flag) + 1])
    arg("--write-game-toml").write_text(PROBED, encoding="utf-8")
    arg("--write-seeds").write_text(SEEDS_PROBED, encoding="utf-8")
    arg("--write-catalog").write_text("{}\n", encoding="utf-8")
    return True, "probed"


class ProbeRefreshOpTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.cue = self.root / "game.cue"
        self.cue.write_text('FILE "game.bin" BINARY\n')

    def tearDown(self):
        self.tmp.cleanup()

    def run_op(self):
        opts = MigrateOptions(disc=str(self.cue), players=2)
        with mock.patch.object(ops, "_run", side_effect=fake_probe):
            return ops.op_probe_disc_refresh(self.root, opts)

    def test_existing_project_is_merged(self):
        (self.root / "game.toml").write_text(EXISTING, encoding="utf-8")
        (self.root / "seeds").mkdir()
        (self.root / "seeds" / "ghidra_funcs.txt").write_text(SEEDS_EXISTING, encoding="utf-8")
        result = self.run_op()
        self.assertTrue(result.ok, result.message)
        toml = (self.root / "game.toml").read_text(encoding="utf-8")
        self.assertIn('plane_nx_sites = ["0x80091390", "0x80091880"]', toml)
        self.assertIn("[prepare_disc]", toml)
        seeds = (self.root / "seeds" / "ghidra_funcs.txt").read_text(encoding="utf-8")
        self.assertTrue(seeds.startswith(SEEDS_EXISTING))

    def test_new_project_takes_probe_output(self):
        result = self.run_op()
        self.assertTrue(result.ok, result.message)
        self.assertEqual((self.root / "game.toml").read_text(encoding="utf-8"), PROBED)
        self.assertEqual(
            (self.root / "seeds" / "ghidra_funcs.txt").read_text(encoding="utf-8"),
            SEEDS_PROBED)


if __name__ == "__main__":
    unittest.main()
