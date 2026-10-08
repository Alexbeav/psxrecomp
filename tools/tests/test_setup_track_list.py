#!/usr/bin/env python3
"""Setup checks the later tracks of a disc against the kit (PS1B-407).

The digests of [prepare_disc] cover the data track. A disc with the right data
track and a later track that is missing or cut short was accepted and staged,
and the game then ran with missing or wrong CD audio. The kit already says
what its disc looks like: [netplay] required_tracks and required_disc_fp, the
fingerprint of the track list (track count, lead-out, each track's type, start
and pregap).

These tests run the CLI's own disc check (`verify-disc`, and the first step of
`generate`) on made-up discs and read the rows it writes. The kit's
fingerprint is made the way a kit's is: by tools/new_project_layout/
probe_disc.py from the complete disc.

No retail data: every track is made-up bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tools"), str(ROOT / "tools" / "new_project_layout")]
import probe_disc  # noqa: E402
import psxrecomp_cli as cli  # noqa: E402
from sdk_progress import ProgressReporter  # noqa: E402

SECTOR = 2352
# The words the setup program takes for the sign of an error line
# (host/psx_cli_tail.h, cli_tail_line_is_error). A warning that held one would
# be shown as the reason of a later stop that has nothing to do with it.
ERROR_SIGNS = ("rror", "ailed", "FAILED", "Traceback", "fatal", "Fatal")

CUE = """FILE "Game (Track 1).bin" BINARY
  TRACK 01 MODE2/2352
    INDEX 01 00:00:00
FILE "Game (Track 2).bin" BINARY
  TRACK 02 AUDIO
    INDEX 00 00:00:00
    INDEX 01 00:02:00
FILE "Game (Track 3).bin" BINARY
  TRACK 03 AUDIO
    INDEX 00 00:00:00
    INDEX 01 00:02:00
"""


def made_up(sectors: int, seed: int) -> bytes:
    return bytes((seed + i * 7) & 0xFF for i in range(251)) * (sectors * SECTOR // 251 + 1)


class SetupTrackList(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # The complete disc: a data track and two audio tracks, one file each.
        self.disc = self.root / "complete"
        self.disc.mkdir()
        self.sizes = {1: 40 * SECTOR, 2: (150 + 60) * SECTOR, 3: (150 + 90) * SECTOR}
        for number, size in self.sizes.items():
            (self.disc / ("Game (Track %d).bin" % number)).write_bytes(made_up(size // SECTOR, number)[:size])
        (self.disc / "Game.cue").write_text(CUE, encoding="ascii")
        self.cue = self.disc / "Game.cue"
        data = (self.disc / "Game (Track 1).bin").read_bytes()
        self.data_sha1 = hashlib.sha1(data).hexdigest()
        # What a kit carries, made from the complete disc as a kit's values are.
        tracks, files = probe_disc.parse_cue(self.cue)
        self.kit_fp = probe_disc.compute_disc_fp(self.cue, tracks, files)
        self.count = 0

    # -- a kit and a player's disc ----------------------------------------------------------------

    def kit(self, setting=None, netplay=True, digests=True, extra_netplay=""):
        self.count += 1
        folder = self.root / ("kit-%d" % self.count)
        folder.mkdir()
        lines = ['[game]', 'id = "TEST-00000"', '[prepare_disc]', 'boot_exe = "TEST_000.00"']
        if digests:
            lines += ['known_sizes = [%d]' % self.sizes[1], 'known_sha1 = ["%s"]' % self.data_sha1]
        if setting is not None:
            lines.append('track_list_mismatch = "%s"' % setting)
        if netplay:
            lines += ['[netplay]', 'require_cue = true', 'required_tracks = 3',
                      'required_disc_fp = "%s"' % self.kit_fp]
        if extra_netplay:
            lines += (['[netplay]'] if not netplay else []) + [extra_netplay]
        config = folder / "game.toml"
        config.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return config

    def copy_of_the_disc(self, name):
        folder = self.root / name
        shutil.copytree(self.disc, folder)
        return folder

    def cut_last_track(self, name="cut-short", by=10 * SECTOR):
        """The wrong tail: the last track ends early. The data track is untouched."""
        folder = self.copy_of_the_disc(name)
        last = folder / "Game (Track 3).bin"
        last.write_bytes(last.read_bytes()[:self.sizes[3] - by])
        return folder / "Game.cue"

    # -- the CLI ----------------------------------------------------------------------------------

    def run_command(self, command, config, disc, skip_hash_check=False, **more):
        rows = io.StringIO()
        arguments = argparse.Namespace(config=str(config), project_root="", disc=str(disc),
                                       skip_hash_check=skip_hash_check, **more)
        with patch.object(cli, "activate_embedded_toolchain", lambda *a, **k: False):
            code = command(arguments, ProgressReporter(json_progress=True, stream=rows,
                                                       log_stream=io.StringIO()))
        return code, [json.loads(line) for line in rows.getvalue().splitlines()]

    def verify(self, config, disc, **more):
        return self.run_command(cli.cmd_verify_disc, config, disc, **more)

    def the_row(self, rows):
        found = [row for row in rows if row["event"] == "track_list"]
        self.assertEqual(len(found), 1, rows)
        return found[0]

    def warnings(self, rows):
        return [row["message"] for row in rows
                if row["event"] == "log" and row.get("message", "").startswith("Track list:")]

    def refusal(self, rows):
        errors = [row for row in rows if row["event"] == "error"]
        self.assertEqual(len(errors), 1, rows)
        self.assertTrue(errors[0].get("verify_failed"))
        return errors[0]["message"]

    # -- the tests --------------------------------------------------------------------------------

    def test_the_complete_disc_is_accepted_and_the_check_says_that_it_ran(self):
        for setting in (None, "warn", "refuse"):
            code, rows = self.verify(self.kit(setting), self.cue)
            self.assertEqual(code, cli.EXIT_OK, rows)
            row = self.the_row(rows)
            self.assertEqual(row["status"], "match")
            self.assertEqual((row["tracks"], row["disc_fp"]), (3, self.kit_fp))
            self.assertEqual(row["leadout"], sum(self.sizes.values()) // SECTOR)
            self.assertEqual(self.warnings(rows), [])
            self.assertTrue(rows[-1]["ok"])
            self.assertEqual(rows[-1]["track_list"]["status"], "match")

    def test_a_last_track_cut_short_is_said_and_the_disc_is_accepted_by_default(self):
        cue = self.cut_last_track()
        for setting in (None, "warn"):
            code, rows = self.verify(self.kit(setting), cue)
            self.assertEqual(code, cli.EXIT_OK, rows)          # the default changes no verdict
            row = self.the_row(rows)
            self.assertEqual((row["status"], row["policy"]), ("mismatch", "warn"))
            self.assertEqual(row["tracks"], 3)
            self.assertEqual(row["leadout"], sum(self.sizes.values()) // SECTOR - 10)
            self.assertNotEqual(row["disc_fp"], self.kit_fp)
            said = self.warnings(rows)
            self.assertEqual(said, [
                'Track list: the data track of "Game.cue" is right, but the disc\'s track list is not '
                "the kit's (its tracks do not begin and end where the kit's do): a track is missing, "
                "cut short or changed, or the .cue belongs to another disc. The game can run; its CD "
                "audio can be missing or wrong."])
            self.assertEqual(row["sentence"], said[0])
            for sign in ERROR_SIGNS:
                self.assertNotIn(sign, said[0])
            self.assertTrue(rows[-1]["ok"])
            self.assertEqual(rows[-1]["track_list"]["status"], "mismatch")

    def test_the_same_disc_is_refused_when_the_kit_says_refuse(self):
        code, rows = self.verify(self.kit("refuse"), self.cut_last_track())
        self.assertEqual(code, cli.EXIT_VERIFY, rows)
        self.assertEqual(self.refusal(rows),
                         'The data track of "Game.cue" is right, but the disc\'s track list is not the '
                         "kit's (its tracks do not begin and end where the kit's do): a track is "
                         "missing, cut short or changed, or the .cue belongs to another disc. Setup "
                         "needs the complete disc.")
        row = self.the_row(rows)
        self.assertEqual((row["status"], row["policy"]), ("mismatch", "refuse"))
        self.assertEqual(self.warnings(rows), [])
        self.assertFalse([r for r in rows if r["event"] == "result"])

    def test_generate_runs_the_same_check_before_it_stages_anything(self):
        cue = self.cut_last_track()
        staged = []
        # Refuse: generate ends at the check, as for a wrong data track.
        with patch.object(cli, "run_prepare_disc", lambda *a, **k: staged.append(a) or cue):
            code, rows = self.run_command(cli.cmd_generate, self.kit("refuse"), cue, force_prepare=False)
        self.assertEqual(code, cli.EXIT_VERIFY, rows)
        self.assertIn("Setup needs the complete disc.", self.refusal(rows))
        self.assertEqual(staged, [])

        # Warn: the sentence is logged and generate goes on to stage the disc.
        class StopHere(Exception):
            pass

        def stop(*a, **k):
            staged.append(a)
            raise StopHere("the test stops generate at the staging step")

        with patch.object(cli, "run_prepare_disc", stop):
            code, rows = self.run_command(cli.cmd_generate, self.kit(), cue, force_prepare=False)
        self.assertEqual(code, cli.EXIT_ERROR, rows)
        self.assertEqual(len(staged), 1)
        self.assertEqual(self.the_row(rows)["status"], "mismatch")
        self.assertEqual(len(self.warnings(rows)), 1)

    def test_what_is_wrong_with_the_tail_is_named(self):
        cases = []
        # A later track file is gone.
        folder = self.copy_of_the_disc("no-track-3")
        (folder / "Game (Track 3).bin").unlink()
        cases.append((folder / "Game.cue", 'the .cue names "Game (Track 3).bin", and that file is not there'))
        # The .cue lists two tracks: another disc's, or a track was left out.
        folder = self.copy_of_the_disc("two-track-cue")
        (folder / "Game.cue").write_text(CUE[:CUE.index('FILE "Game (Track 3).bin"')], encoding="ascii")
        cases.append((folder / "Game.cue", "it has 2 tracks, the kit's disc has 3"))
        # The data track was selected by itself: the other tracks are not staged.
        cases.append((self.disc / "Game (Track 1).bin", "it has 1 track, the kit's disc has 3"))
        # A later track cut inside a sector.
        cases.append((self.cut_last_track("cut-inside-a-sector", by=1000),
                      '"Game (Track 3).bin" is %d bytes, which is not a whole number of sectors'
                      % (self.sizes[3] - 1000)))
        for disc, detail in cases:
            code, rows = self.verify(self.kit(), disc)
            self.assertEqual(code, cli.EXIT_OK, rows)
            row = self.the_row(rows)
            self.assertEqual((row["status"], row["detail"]), ("mismatch", detail))
            self.assertEqual(self.warnings(rows), [
                'Track list: the data track of "%s" is right, but the disc\'s track list is not the '
                "kit's (%s): a track is missing, cut short or changed, or the .cue belongs to another "
                "disc. The game can run; its CD audio can be missing or wrong." % (disc.name, detail)])
            code, rows = self.verify(self.kit("refuse"), disc)
            self.assertEqual(code, cli.EXIT_VERIFY, rows)
            self.assertIn("(%s)" % detail, self.refusal(rows))

    def test_a_wrong_data_track_is_refused_as_before_whatever_the_setting(self):
        folder = self.copy_of_the_disc("other-data")
        (folder / "Game (Track 1).bin").write_bytes(made_up(40, 99)[:self.sizes[1]])
        for setting in (None, "warn", "refuse"):
            code, rows = self.verify(self.kit(setting), folder / "Game.cue")
            self.assertEqual(code, cli.EXIT_VERIFY, rows)
            self.assertTrue(self.refusal(rows).startswith("disc digests not in prepare_disc.known_*"))
            self.assertFalse([row for row in rows if row["event"] == "track_list"])

    def test_what_the_track_list_cannot_see(self):
        # A later track with the right length and other content has the same
        # track list. The check does not see it; only a digest per track would.
        folder = self.copy_of_the_disc("other-audio")
        (folder / "Game (Track 3).bin").write_bytes(made_up(240, 77)[:self.sizes[3]])
        code, rows = self.verify(self.kit("refuse"), folder / "Game.cue")
        self.assertEqual(code, cli.EXIT_OK, rows)
        self.assertEqual(self.the_row(rows)["status"], "match")

    def test_a_kit_without_a_track_list_is_not_checked_and_the_row_says_so(self):
        code, rows = self.verify(self.kit("refuse", netplay=False), self.cut_last_track())
        self.assertEqual(code, cli.EXIT_OK, rows)
        row = self.the_row(rows)
        self.assertEqual(row["status"], "not_checked")
        self.assertIn("the kit lists no track list", row["reason"])
        self.assertEqual(self.warnings(rows), [])

    def test_a_kit_that_gives_only_the_track_count(self):
        config = self.kit("refuse", netplay=False, extra_netplay="required_tracks = 3")
        code, rows = self.verify(config, self.cut_last_track())    # three tracks: the count is right
        self.assertEqual(code, cli.EXIT_OK, rows)
        self.assertEqual(self.the_row(rows)["status"], "match")
        code, rows = self.verify(config, self.disc / "Game (Track 1).bin")
        self.assertEqual(code, cli.EXIT_VERIFY, rows)
        self.assertIn("(it has 1 track, the kit's disc has 3)", self.refusal(rows))

    def test_a_set_of_discs_lists_a_fingerprint_for_each(self):
        other = "0" * 64
        config = self.kit("refuse", netplay=False, extra_netplay=(
            'required_disc_fp = "%s"\nrequired_disc_fps = ["%s", "%s"]' % (other, other, self.kit_fp)))
        code, rows = self.verify(config, self.cue)
        self.assertEqual(code, cli.EXIT_OK, rows)
        self.assertEqual(self.the_row(rows)["status"], "match")
        code, rows = self.verify(config, self.cut_last_track())
        self.assertEqual(code, cli.EXIT_VERIFY, rows)

    def test_a_setting_that_is_not_a_known_word_warns_and_names_the_kits_fault(self):
        code, rows = self.verify(self.kit("block"), self.cut_last_track())
        self.assertEqual(code, cli.EXIT_OK, rows)
        row = self.the_row(rows)
        self.assertEqual((row["status"], row["policy"]), ("mismatch", "warn"))
        faults = [r["message"] for r in rows if r["event"] == "log" and r["message"].startswith("KIT FAULT:")]
        self.assertEqual(faults, ['KIT FAULT: [prepare_disc] track_list_mismatch = "block" is not "warn" or '
                                  '"refuse"; "warn" is used'])
        self.assertEqual(len(self.warnings(rows)), 1)

    def test_skip_hash_check_skips_the_track_list_too(self):
        cue = self.cut_last_track()
        code, rows = self.verify(self.kit("refuse"), cue)
        self.assertEqual(code, cli.EXIT_VERIFY, rows)
        code, rows = self.verify(self.kit("refuse"), cue, skip_hash_check=True)
        self.assertEqual(code, cli.EXIT_OK, rows)
        row = self.the_row(rows)
        self.assertEqual((row["status"], row["reason"]), ("not_checked", "--skip-hash-check"))

    def test_a_kit_without_digests_is_still_checked_and_the_sentence_claims_less(self):
        code, rows = self.verify(self.kit(digests=False), self.cut_last_track())
        self.assertEqual(code, cli.EXIT_OK, rows)
        said = self.warnings(rows)
        self.assertEqual(len(said), 1)
        self.assertTrue(said[0].startswith('Track list: the track list of "Game.cue" is not the kit\'s ('), said[0])
        self.assertNotIn("data track", said[0].split("(")[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
