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
import os
from pathlib import Path
import shutil
import struct
import subprocess
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
            # Setup cannot tell another pressing from a damaged copy or from
            # the right disc in a form it does not read: the sentence names all.
            self.assertIn("another pressing, revision or region of the game (a different disc)", text)
            self.assertIn("a damaged copy", text)
            self.assertIn("the right disc in a form setup cannot read", text)
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

    # ---- one file with every track and no list of the tracks -------------

    def lone_bin(self, cue_text=None, folder="lone"):
        lone = self.root / folder
        lone.mkdir()
        (lone / "Game.bin").write_bytes(self.track1 + self.audio)
        if cue_text is not None:
            (lone / "Game.cue").write_text(cue_text, encoding="ascii")
        return lone / "Game.bin"

    ONE_TRACK_CUE = 'FILE "Game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
    ASK_FOR_CUE = "Select the .cue file that belongs to Game.bin"

    def test_one_file_with_no_cue_is_refused_and_asks_for_the_cue(self):
        picked = self.lone_bin()
        with self.assertRaises(cli.DiscVerifyError) as raised:
            self.verify(picked)
        text = str(raised.exception)
        self.assertIn(self.ASK_FOR_CUE, text)
        self.assertEqual(text.count(". "), 0, "one sentence")
        # Never staged as a one-track disc over the whole file.
        out = self.root / "out-lone"
        code, log = self.prepare(picked, out)
        self.assertEqual(code, 1, log)
        self.assertIn(self.ASK_FOR_CUE, log)
        self.assertFalse(out.exists())

    def test_a_cue_that_calls_the_whole_file_one_track_is_no_track_list(self):
        picked = self.lone_bin(self.ONE_TRACK_CUE)
        for selected in (picked, picked.with_suffix(".cue")):
            with self.assertRaises(cli.DiscVerifyError) as raised:
                self.verify(selected)
            self.assertIn(self.ASK_FOR_CUE, str(raised.exception))
            out = self.root / f"out-{selected.suffix[1:]}"
            code, log = self.prepare(selected, out)
            self.assertEqual(code, 1, log)
            self.assertFalse(out.exists())

    def test_one_file_with_no_cue_is_staged_whole_from_the_kits_track_list(self):
        sizes = f"[{len(self.track1)}, {len(self.audio)}]"
        self.config.write_text(self.config.read_text(encoding="utf-8")
                               + f"track_sizes = {sizes}\ntrack_pregaps = [0, 20]\n", encoding="utf-8")
        prep = dict(self.prep, track_sizes=[len(self.track1), len(self.audio)], track_pregaps=[0, 20])
        for folder, cue_text in (("bare", None), ("one-track-cue", self.ONE_TRACK_CUE)):
            with self.subTest(folder=folder):
                picked = self.lone_bin(cue_text, folder)
                identity = cli.verify_disc_path(picked, prep, skip_hash=False, progress=quiet())
                self.assertTrue(identity["verified"])
                self.assertEqual(identity["sha1"], self.sha1)
                out = self.root / f"out-{folder}"
                code, log = self.prepare(picked, out)
                self.assertEqual(code, 0, log)
                self.assertIn("track list of 2 track(s) taken from the kit", log)
                # The same table the disc's own cue gives.
                self.assertEqual((out / "Staged.cue").read_text(encoding="utf-8"),
                                 self.single_cue.read_text(encoding="ascii"))
                self.assertEqual((out / "Game.bin").read_bytes(), self.track1 + self.audio)
                self.assertTrue((out / BOOT).is_file())
                receipt = json.loads((out / "Staged.disc-receipt.json").read_text(encoding="utf-8"))
                self.assertEqual(receipt["source_data_track"]["size"], len(self.track1))

    def test_the_kits_track_list_is_used_only_for_a_file_it_fits(self):
        t1, total = 300 * SECTOR, 1000 * SECTOR
        table = disc_forms.kit_track_table({"track_sizes": [t1, 400 * SECTOR, 300 * SECTOR]}, total, t1)
        # Track 1 is data; later tracks are audio with the 150-frame pregap.
        self.assertEqual(table, [(1, False, 0, 0, 0), (2, True, 300, 450, 0), (3, True, 700, 850, 0)])
        self.assertEqual(disc_forms.rebuilt_cue("Game.bin", table),
                         'FILE "Game.bin" BINARY\n'
                         "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                         "  TRACK 02 AUDIO\n    INDEX 00 00:04:00\n    INDEX 01 00:06:00\n"
                         "  TRACK 03 AUDIO\n    INDEX 00 00:09:25\n    INDEX 01 00:11:25\n")
        refused = [
            {},                                                        # no list
            {"track_sizes": [total]},                                  # one track is no list
            {"track_sizes": [t1, 400 * SECTOR, 299 * SECTOR]},         # does not add up
            {"track_sizes": [t1 + SECTOR, 400 * SECTOR, 299 * SECTOR]},  # another data track
            {"track_sizes": [t1, 400 * SECTOR + 1, 300 * SECTOR - 1]},   # not whole sectors
            {"track_sizes": [t1, 700 * SECTOR], "track_pregaps": [0]},   # lists of unequal length
            {"track_sizes": [t1, 700 * SECTOR], "track_pregaps": [0, 700]},  # pregap fills the track
            {"track_sizes": [t1, "700"]},                              # not numbers
            {"track_sizes": [t1, 700 * SECTOR], "track_pregaps": [150, 150]},  # a pregap on the data track
            {"track_sizes": [t1, 700 * SECTOR], "track_counts": [3]},    # counts that do not add up
            {"track_sizes": [t1, 700 * SECTOR], "track_counts": [1, 1]},  # no disc of two tracks or more
        ]
        for prep in refused:
            self.assertIsNone(disc_forms.kit_track_table(prep, total, t1), prep)

    def test_the_kits_track_list_of_a_set_and_of_unusual_pregaps(self):
        # A set: the tracks of every disc in disc order, and how many belong to each.
        a, b = [300 * SECTOR, 700 * SECTOR], [500 * SECTOR, 200 * SECTOR, 400 * SECTOR]
        prep = {"track_sizes": a + b, "track_counts": [2, 3]}
        self.assertEqual(disc_forms.kit_track_table(prep, sum(a), a[0]),
                         [(1, False, 0, 0, 0), (2, True, 300, 450, 0)])
        self.assertEqual(disc_forms.kit_track_table(prep, sum(b), b[0]),
                         [(1, False, 0, 0, 0), (2, True, 500, 650, 0), (3, True, 700, 850, 0)])
        # Recognised by the whole image, with no data track named: the sum decides.
        self.assertEqual(len(disc_forms.kit_track_table(prep, sum(b))), 3)
        self.assertIsNone(disc_forms.kit_track_table(prep, sum(a) + SECTOR))
        self.assertIsNone(disc_forms.kit_track_table(prep, sum(a), b[0]))
        # Pregaps per track, in frames: another length than 150, none, and one
        # that is not in the file (negative: the cue's PREGAP line).
        prep = {"track_sizes": [300 * SECTOR, 1000 * SECTOR, 200 * SECTOR, 200 * SECTOR],
                "track_pregaps": [0, 833, 0, -150]}
        table = disc_forms.kit_track_table(prep, 1700 * SECTOR, 300 * SECTOR)
        self.assertEqual(table, [(1, False, 0, 0, 0), (2, True, 300, 1133, 0),
                                 (3, True, 1300, 1300, 0), (4, True, 1500, 1500, 150)])
        self.assertEqual(disc_forms.rebuilt_cue("Game.bin", table),
                         'FILE "Game.bin" BINARY\n'
                         "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                         "  TRACK 02 AUDIO\n    INDEX 00 00:04:00\n    INDEX 01 00:15:08\n"
                         "  TRACK 03 AUDIO\n    INDEX 01 00:17:25\n"
                         "  TRACK 04 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:20:00\n")

    def test_a_list_is_tied_to_its_image_by_more_than_the_total_length(self):
        t1, total = 300 * SECTOR, 1000 * SECTOR
        listed = {"known_sizes": [t1, total], "known_sha1": ["a", "b"]}
        # Two lists of one length with other track borders: no answer, by
        # either way into the list.
        two = dict(listed, track_sizes=[t1, 700 * SECTOR, t1, 400 * SECTOR, 300 * SECTOR], track_counts=[2, 3])
        self.assertIsNone(disc_forms.kit_track_table(two, total))
        self.assertIsNone(disc_forms.kit_track_table(two, total, t1))
        self.assertTrue(disc_forms.kit_list_misfit(two, total))
        # The same table twice is one answer.
        twice = dict(listed, track_sizes=[t1, 700 * SECTOR, t1, 700 * SECTOR], track_counts=[2, 2])
        self.assertEqual(len(disc_forms.kit_track_table(twice, total)), 2)
        # A whole image: the list's first track must be a data track the kit lists.
        other = dict(listed, track_sizes=[t1 + SECTOR, 699 * SECTOR])
        self.assertIsNone(disc_forms.kit_track_table(other, total))
        self.assertTrue(disc_forms.kit_list_misfit(other, total))
        # Keys that do not agree with each other are a misfit for every file.
        self.assertTrue(disc_forms.kit_list_misfit(dict(listed, track_sizes=[t1, 700 * SECTOR], track_counts=[3]), total))
        # No keys, a fitting list, and a lone data track are not.
        self.assertFalse(disc_forms.kit_list_misfit(listed, total))
        self.assertFalse(disc_forms.kit_list_misfit(dict(listed, track_sizes=[t1, 700 * SECTOR]), total))
        self.assertFalse(disc_forms.kit_list_misfit(dict(listed, track_sizes=[t1, 700 * SECTOR]), t1))

    def test_a_file_name_outside_ascii_reaches_the_setup_window_whole(self):
        # PS1B-413. The refusal carries the player's file name. The CLI writes
        # its rows as ASCII JSON, so a Greek letter is \uXXXX there; the setup
        # host must turn that back into the letter, not into "u0394".
        import argparse
        name = "Δίσκος παιχνιδιού (Track 1).bin"
        folder = self.root / "greek"
        folder.mkdir()
        (folder / name).write_bytes(prepare.iso_to_bin(cooked_disc(b"REV1")))
        (folder / "disc.cue").write_text(
            f'FILE "{name}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n', encoding="utf-8")
        self.config.write_text(self.config.read_text(encoding="utf-8")
                               + 'cue_name = "Game (Europe).cue"\n', encoding="utf-8")
        rows = io.StringIO()
        args = argparse.Namespace(config=str(self.config), project_root="",
                                  disc=str(folder / "disc.cue"), skip_hash_check=False)
        code = cli.cmd_verify_disc(args, ProgressReporter(json_progress=True, stream=rows,
                                                         log_stream=io.StringIO()))
        self.assertEqual(code, 3)
        line = rows.getvalue().splitlines()[-1]
        message = json.loads(line)["message"]
        self.assertIn(f"The selected file {name} is", message)
        line.encode("ascii")                      # the row itself is ASCII
        self.assertIn("\\u0394", line)

        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if cc is None:
            self.skipTest("no C compiler on PATH: the host's reader was not run")
        harness = self.root / "read_row.c"
        harness.write_text(
            '#include "psx_json_text.h"\n#include <stdlib.h>\n'
            "int main(int argc, char** argv) {\n"
            "    static char line[16384], out[4096];\n"
            '    FILE* f = fopen(argv[1], "rb");\n'
            "    size_t n = f ? fread(line, 1, sizeof(line) - 1, f) : 0;\n"
            "    size_t cap = argc > 2 ? (size_t)atoi(argv[2]) : sizeof(out);\n"
            "    line[n] = 0;\n"
            '    if (!json_get_string(line, "message", out, cap)) return 3;\n'
            "    fwrite(out, 1, strlen(out), stdout);\n"
            "    return 0;\n}\n", encoding="ascii")
        exe = self.root / ("read_row.exe" if os.name == "nt" else "read_row")
        built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-o", str(exe), str(harness),
                                "-I", str(ROOT / "host")], capture_output=True)
        self.assertEqual(built.returncode, 0, built.stderr.decode("utf-8", "replace")[-1500:])
        self.assertNotIn(b"psx_json_text.h", built.stderr)   # no warning in the reader
        row = self.root / "row.json"
        row.write_bytes(line.encode("ascii"))
        shown = subprocess.run([str(exe), str(row)], capture_output=True).stdout.decode("utf-8")
        self.assertEqual(shown, message)
        self.assertNotIn("u0394", shown)
        # Cut at the room, never inside a letter: every length decodes.
        for cap in range(150, 200):
            cut = subprocess.run([str(exe), str(row), str(cap)], capture_output=True).stdout
            self.assertEqual(cut.decode("utf-8"), message[:len(cut.decode("utf-8"))], cap)
            self.assertLess(len(cut), cap)
        # The other escapes: a quote, a backslash, a line break (a space in the
        # window), and a letter outside the first plane.
        row.write_bytes(b'{"event":"error","message":"a \\"b\\" c\\\\d\\ne \\ud83c\\udfae f \\ud83c g"}')
        self.assertEqual(subprocess.run([str(exe), str(row)], capture_output=True).stdout.decode("utf-8"),
                         'a "b" c\\d e \U0001F3AE f ? g')

    def test_a_listed_whole_disc_image_with_no_cue_is_staged_with_the_kits_track_list(self):
        # A kit that lists the whole-disc image too: the one file is accepted
        # as it always was. With no cue it was staged as one track, without CD
        # audio; with the kit's track list it is staged whole.
        whole = self.track1 + self.audio
        base = self.config.read_text(encoding="utf-8").replace(
            f"known_sizes = [{len(self.track1)}]", f"known_sizes = [{len(self.track1)}, {len(whole)}]").replace(
            f'known_sha1 = ["{self.sha1}"]', f'known_sha1 = ["{self.sha1}", "{hashlib.sha1(whole).hexdigest()}"]').replace(
            f'known_md5 = ["{self.md5}"]', f'known_md5 = ["{self.md5}", "{hashlib.md5(whole).hexdigest()}"]')
        self.assertIn(str(len(whole)), base)
        # Until the kit carries the key, today's behaviour: one track.
        self.config.write_text(base, encoding="utf-8")
        picked = self.lone_bin(None, "whole-no-list")
        out = self.root / "out-whole-no-list"
        code, log = self.prepare(picked, out)
        self.assertEqual(code, 0, log)
        self.assertNotIn("taken from the kit", log)
        self.assertEqual((out / "Staged.cue").read_text(encoding="utf-8").count("TRACK"), 1)
        # With the kit's list: the same table the disc's own cue gives.
        self.config.write_text(base + f"track_sizes = [{len(self.track1)}, {len(self.audio)}]\n"
                               "track_pregaps = [0, 20]\n", encoding="utf-8")
        picked = self.lone_bin(None, "whole-with-list")
        out = self.root / "out-whole-with-list"
        code, log = self.prepare(picked, out)
        self.assertEqual(code, 0, log)
        self.assertIn("track list of 2 track(s) taken from the kit", log)
        self.assertEqual((out / "Staged.cue").read_text(encoding="utf-8"),
                         self.single_cue.read_text(encoding="ascii"))
        self.assertEqual((out / "Game.bin").read_bytes(), whole)
        self.assertTrue((out / BOOT).is_file())
        # A list that fits no image the kit lists is the kit's fault. The
        # player is not refused: the file is staged as before, and the log
        # says that the kit's values are wrong.
        self.config.write_text(base + f"track_sizes = [{len(self.track1)}, {len(self.audio) - SECTOR}]\n"
                               "track_pregaps = [0, 20]\n", encoding="utf-8")
        picked = self.lone_bin(None, "whole-wrong-list")
        out = self.root / "out-whole-wrong-list"
        code, log = self.prepare(picked, out)
        self.assertEqual(code, 0, log)
        self.assertIn("KIT FAULT: [prepare_disc] track_sizes describes no image of", log)
        self.assertEqual((out / "Staged.cue").read_text(encoding="utf-8").count("TRACK"), 1)
        # A lone data track is not a misfit of the list.
        out = self.root / "out-track-1-alone"
        (self.root / "t1").mkdir()
        (self.root / "t1" / "Game (Track 1).bin").write_bytes(self.track1)
        code, log = self.prepare(self.root / "t1" / "Game (Track 1).bin", out)
        self.assertEqual(code, 0, log)
        self.assertNotIn("KIT FAULT", log)
        self.config.write_text(base + f"track_sizes = [{len(self.track1)}, {len(self.audio)}]\n"
                               "track_pregaps = [0, 20]\n", encoding="utf-8")
        # A cue that lists the tracks still wins over the kit's list.
        out = self.root / "out-whole-with-cue"
        code, log = self.prepare(self.single_cue, out)
        self.assertEqual(code, 0, log)
        self.assertNotIn("taken from the kit", log)
        self.assertEqual((out / "Staged.cue").read_text(encoding="utf-8").count("TRACK"), 2)

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
