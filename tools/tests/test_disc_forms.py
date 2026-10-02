"""One disc in its file forms at setup (PS1G-63). Source-owned fixtures: no
retail sectors or executables.

A kit lists the data track of its disc. A correct disc can reach setup as one
file per track (the Redump layout) or as one file with every track (what
``chdman extractcd`` writes). Both are the same disc and both must pass; a
disc with other bytes must be refused with a sentence that says what the kit
needs and what was selected.
"""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tools")]
import disc_forms
import prepare_disc as prepare
import psxrecomp_cli as cli
from sdk_progress import ProgressReporter

BOOT = "SLES_000.00"
SECTOR = 2352


def cooked_disc(marker: bytes = b"") -> bytes:
    sectors = [bytearray(2048) for _ in range(24)]
    sectors[16][1:6] = b"CD001"
    struct.pack_into("<I", sectors[16], 158, 20)
    struct.pack_into("<I", sectors[16], 166, 2048)
    cursor = 0
    for name, lba, data in [("SYSTEM.CNF", 21, b"BOOT = cdrom:\\" + BOOT.encode() + b";1"),
                            (BOOT, 22, b"PS-X EXE" + bytes(2040))]:
        encoded = (name + ";1").encode()
        record = bytearray(33 + len(encoded) + (len(encoded) % 2 == 0))
        record[0] = len(record)
        struct.pack_into("<I", record, 2, lba)
        struct.pack_into("<I", record, 10, len(data))
        record[32] = len(encoded)
        record[33:33 + len(encoded)] = encoded
        sectors[20][cursor:cursor + len(record)] = record
        cursor += len(record)
        sectors[lba][:len(data)] = data
    sectors[23][:len(marker)] = marker   # a sector no file uses: the "pressing"
    return b"".join(sectors)


def quiet():
    return ProgressReporter(stream=io.StringIO(), log_stream=io.StringIO())


class DiscFormsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.track1 = prepare.iso_to_bin(cooked_disc())
        self.audio = bytes(range(1, 241)) * (SECTOR * 40 // 240)
        self.assertEqual(len(self.track1) % SECTOR, 0)
        self.assertEqual(len(self.audio), SECTOR * 40)
        self.sha1 = hashlib.sha1(self.track1).hexdigest()
        self.md5 = hashlib.md5(self.track1).hexdigest()
        self.prep = {
            "cue_name": "Game (Europe).cue",
            "known_sizes": [len(self.track1)],
            "known_md5": [self.md5],
            "known_sha1": [self.sha1],
        }
        # The Redump layout: one file per track.
        self.multi = self.root / "multi"
        self.multi.mkdir()
        (self.multi / "Game (Track 1).bin").write_bytes(self.track1)
        (self.multi / "Game (Track 2).bin").write_bytes(self.audio)
        self.multi_cue = self.multi / "Game.cue"
        self.multi_cue.write_text(
            'FILE "Game (Track 1).bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
            'FILE "Game (Track 2).bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n'
            '    INDEX 01 00:00:20\n', encoding="ascii")
        # The same disc as one file with both tracks.
        self.single = self.root / "single"
        self.single.mkdir()
        self.single_bin = self.single / "Game.bin"
        self.single_bin.write_bytes(self.track1 + self.audio)
        self.single_cue = self.single / "Game.cue"
        self.single_cue.write_text(
            'FILE "Game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
            '  TRACK 02 AUDIO\n    INDEX 00 00:00:24\n    INDEX 01 00:00:44\n', encoding="ascii")
        self.config = self.root / "game.toml"
        self.config.write_text(
            f'[game]\nid = "SLES-00000"\n[prepare_disc]\nboot_exe = "{BOOT}"\n'
            f'known_sizes = [{len(self.track1)}]\nknown_md5 = ["{self.md5}"]\n'
            f'known_sha1 = ["{self.sha1}"]\n', encoding="utf-8")

    def verify(self, disc):
        return cli.verify_disc_path(disc, self.prep, skip_hash=False, progress=quiet())

    def prepare(self, source, out):
        output = io.StringIO()
        args = ["prepare_disc.py", str(source), "--config", str(self.config),
                "--out-dir", str(out), "--cue-name", "Staged.cue"]
        with patch.object(sys, "argv", args), contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output), patch.object(prepare, "_configure_stdio"):
            code = prepare.main()
        return code, output.getvalue()

    # ---- the setup check -------------------------------------------------

    def test_one_file_per_track_is_accepted_as_before(self):
        identity = self.verify(self.multi_cue)
        self.assertTrue(identity["verified"])
        self.assertEqual(identity["sha1"], self.sha1)
        self.assertEqual(identity["size"], len(self.track1))
        self.assertNotIn("single_bin", identity)

    def test_one_file_with_every_track_is_accepted(self):
        for picked in (self.single_cue, self.single_bin):
            identity = self.verify(picked)
            self.assertTrue(identity["verified"], picked.name)
            # The identity is the data track's, as in the other layout.
            self.assertEqual(identity["sha1"], self.sha1)
            self.assertEqual(identity["md5"], self.md5)
            self.assertEqual(identity["size"], len(self.track1))
            self.assertEqual(identity["single_bin"],
                             {"image_size": len(self.track1) + len(self.audio)})

    def test_another_pressing_is_refused_with_a_sentence_that_names_both(self):
        other = prepare.iso_to_bin(cooked_disc(b"REV1"))
        self.assertEqual(len(other), len(self.track1))
        other_sha1 = hashlib.sha1(other).hexdigest()
        self.single_bin.write_bytes(other + self.audio)
        (self.multi / "Game (Track 1).bin").write_bytes(other)
        for picked, name, size in ((self.single_cue, "Game.bin", len(other) + len(self.audio)),
                                   (self.multi_cue, "Game (Track 1).bin", len(other))):
            with self.assertRaises(cli.DiscVerifyError) as raised:
                self.verify(picked)
            text = str(raised.exception)
            self.assertIn("not the disc image this kit was made from", text)
            self.assertIn(f"Game (Europe) (data track {len(self.track1):,} bytes)", text)
            self.assertIn(f"The selected file {name} is {size:,} bytes", text)
            self.assertIn("is a different disc", text)
            self.assertNotIn("wrong dump", text)
            # One line of it is shown in the setup window.
            self.assertLess(len(text), 400)
        # The data track of the refused single file is the other pressing's.
        self.assertNotEqual(other_sha1, self.sha1)

    def test_refusal_reaches_the_setup_window_as_an_error_event(self):
        import argparse
        (self.multi / "Game (Track 1).bin").write_bytes(prepare.iso_to_bin(cooked_disc(b"REV1")))
        self.config.write_text(self.config.read_text(encoding="utf-8")
                               + 'cue_name = "Game (Europe).cue"\n', encoding="utf-8")
        output = io.StringIO()
        args = argparse.Namespace(config=str(self.config), project_root="",
                                  disc=str(self.multi_cue), skip_hash_check=False)
        code = cli.cmd_verify_disc(args, ProgressReporter(json_progress=True, stream=output,
                                                         log_stream=io.StringIO()))
        self.assertEqual(code, 3)
        events = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(events[-1]["event"], "error")
        self.assertTrue(events[-1]["verify_failed"])
        self.assertIn("The kit needs Game (Europe)", events[-1]["message"])
        # The digests stay in the log for tools.
        self.assertTrue(any("disc digests not in prepare_disc.known_*" in str(e.get("message", ""))
                            for e in events[:-1]))

    def test_a_longer_file_is_never_accepted_on_its_size_alone(self):
        sizes_only = {"known_sizes": [len(self.track1)]}
        self.assertIsNone(disc_forms.track_in_single_bin(
            self.single_bin, self.single_bin.stat().st_size, disc_forms.known_images(sizes_only)))
        with self.assertRaises(cli.DiscVerifyError):
            cli.verify_disc_path(self.single_bin, sizes_only, skip_hash=False, progress=quiet())

    def test_only_whole_sectors_after_a_listed_track_count(self):
        known = disc_forms.known_images(self.prep)
        ragged = self.root / "ragged.bin"
        ragged.write_bytes(self.track1 + self.audio + b"x")
        self.assertIsNone(disc_forms.track_in_single_bin(ragged, ragged.stat().st_size, known))
        # The listed track itself is the whole-file case, not this one.
        exact = self.multi / "Game (Track 1).bin"
        self.assertIsNone(disc_forms.track_in_single_bin(exact, len(self.track1), known))
        self.assertEqual(disc_forms.track_in_single_bin(
            self.single_bin, self.single_bin.stat().st_size, known),
            (len(self.track1), self.md5, self.sha1))

    def test_prefix_digests_stop_at_the_end_of_the_file(self):
        got = disc_forms.prefix_digests(self.single_bin, [SECTOR, len(self.track1), 10 ** 9, 0])
        self.assertEqual(sorted(got), [SECTOR, len(self.track1)])
        self.assertEqual(got[len(self.track1)], (self.md5, self.sha1))
        self.assertEqual(got[SECTOR][1], hashlib.sha1(self.track1[:SECTOR]).hexdigest())

    def test_known_images_pair_the_lists_by_position(self):
        self.assertEqual(disc_forms.known_images({"known_sizes": [1, 2], "known_sha1": ["AB"]}),
                         [(1, "", "ab"), (2, "", "")])
        self.assertEqual(disc_forms.known_images({}), [])

    # ---- staging ---------------------------------------------------------

    def test_staging_one_file_keeps_its_audio_track(self):
        for picked in (self.single_cue, self.single_bin):
            out = self.root / f"out-{picked.suffix[1:]}"
            code, log = self.prepare(picked, out)
            self.assertEqual(code, 0, log)
            self.assertIn("one file holds every track", log)
            cue = (out / "Staged.cue").read_text(encoding="utf-8")
            self.assertEqual(cue.count("TRACK"), 2, cue)
            self.assertIn('FILE "Game.bin" BINARY', cue)
            self.assertIn("TRACK 02 AUDIO", cue)
            self.assertEqual((out / "Game.bin").read_bytes(), self.track1 + self.audio)
            self.assertTrue((out / BOOT).is_file())
            receipt = json.loads((out / "Staged.disc-receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["source_data_track"]["sha1"], self.sha1)
            self.assertEqual(receipt["source_data_track"]["size"], len(self.track1))

    def test_staging_one_file_without_a_cue_says_the_audio_is_lost(self):
        lone = self.root / "lone"
        lone.mkdir()
        (lone / "Game.bin").write_bytes(self.track1 + self.audio)
        out = self.root / "out-lone"
        code, log = self.prepare(lone / "Game.bin", out)
        self.assertEqual(code, 0, log)
        self.assertIn("no cue beside this file", log)
        self.assertEqual((out / "Staged.cue").read_text(encoding="utf-8").count("TRACK"), 1)

    def test_staging_one_file_per_track_is_unchanged(self):
        out = self.root / "out-multi"
        code, log = self.prepare(self.multi_cue, out)
        self.assertEqual(code, 0, log)
        self.assertNotIn("one file holds every track", log)
        self.assertEqual((out / "Game (Track 2).bin").read_bytes(), self.audio)
        self.assertEqual((out / "Staged.cue").read_text(encoding="utf-8").count("FILE"), 2)

    def test_a_track_file_picked_from_a_set_is_staged_with_the_set(self):
        out = self.root / "out-track1"
        code, log = self.prepare(self.multi / "Game (Track 1).bin", out)
        self.assertEqual(code, 0, log)
        self.assertEqual((out / "Game (Track 2).bin").read_bytes(), self.audio)
        # A cue that names a file that is not there is left alone.
        (self.multi / "Game (Track 2).bin").unlink()
        self.assertIsNone(prepare.owning_cue(self.multi / "Game (Track 1).bin"))

    def test_another_pressing_is_not_staged(self):
        self.single_bin.write_bytes(prepare.iso_to_bin(cooked_disc(b"REV1")) + self.audio)
        out = self.root / "out-other"
        code, log = self.prepare(self.single_cue, out)
        self.assertEqual(code, 1, log)
        self.assertIn("source digests are not in prepare_disc.known_*", log)
        self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
