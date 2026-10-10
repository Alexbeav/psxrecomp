#!/usr/bin/env python3
"""What the CLI says about a player's file is what the setup window gets (PS1B-413).

The setup host has no console. It reads the CLI's progress rows, one JSON
object on a line, and shows their "message" in the setup window. When the CLI
ends with an error, the last error message is the reason on the screen. The
CLI writes ASCII JSON, so a letter outside ASCII is \\uXXXX in a row.

These tests compile the host's own reader (host/psx_json_text.h) and its
failure text (host/psx_cli_tail.h) into a small program and give it the rows
the CLI wrote for a disc that lies in a folder with a Greek name.

No retail data: the disc is a made-up file and the SBI rule is a fixture.

Usage: test_setup_host_cli_text.py [--compiler <cc>]
Exit 77 (skipped, not passed) when no C compiler is found.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tools")]
import disc_companion as dc  # noqa: E402
import psxrecomp_cli as cli  # noqa: E402
from sdk_progress import ProgressReporter  # noqa: E402

COMPILER = None

# The room the launcher gives the host for a failure text today (recomp-ui,
# launcher_model.c: char err[256]), and the room of the larger setup window
# (640 bytes) that waits on a launcher change.
LAUNCHER_ROOM = 256
LARGE_ROOM = 640
# A folder as a player has it. The length is fixed so that the refusal below
# fits the host's 480-byte line as text and does not fit it with every Greek
# letter spelled as six ASCII characters.
PLAYER_FOLDER = "C:\\Users\\Αλέξανδρος\\Παιχνίδια\\Resident Evil 3 - Nemesis (Ευρώπη)"
DISC_NAME = "Δίσκος παιχνιδιού"
LAST_SENTENCE = "Setup does not supply or download SBI files."

HARNESS = r"""
#include "psx_cli_tail.h"
#include <stdlib.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

/* host_text value <rows> <key> <room>
 *     the value of <key> in the first row that has it, read into <room> bytes
 * host_text fail <rows> <exit code> <room> <label>
 *     what the setup window shows after the CLI wrote <rows> and ended with
 *     <exit code>: every line goes through cli_tail_note, then cli_fail_msg
 *     writes into <room> bytes
 * Exit 3: no value. Exit 4: a byte was written outside the room. */
static char g_rows[262144];

static char* room_new(size_t room) {
    char* p = (char*)malloc(room + 16);
    if (!p) exit(5);
    memset(p, 0x7E, room + 16);
    return p;
}

static int room_kept(const char* p, size_t room) {
    size_t i;
    for (i = 0; i < 16; ++i)
        if (p[room + i] != 0x7E) return 0;
    return strlen(p) < room;
}

int main(int argc, char** argv) {
    FILE* f;
    size_t n, room;
    char* out;
    if (argc < 5) return 2;
    f = fopen(argv[2], "rb");
    if (!f) return 2;
    n = fread(g_rows, 1, sizeof(g_rows) - 1, f);
    fclose(f);
    g_rows[n] = '\0';
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (strcmp(argv[1], "value") == 0) {
        room = (size_t)atoi(argv[4]);
        out = room_new(room);
        if (!json_get_string(g_rows, argv[3], out, room)) return 3;
        if (!room_kept(out, room)) return 4;
        fwrite(out, 1, strlen(out), stdout);
        return 0;
    }
    if (strcmp(argv[1], "fail") == 0 && argc >= 6) {
        CliTail tail;
        char* line = g_rows;
        memset(&tail, 0, sizeof(tail));
        while (*line) {
            char* end = strchr(line, '\n');
            if (end) *end = '\0';
            if (*line && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = '\0';
            cli_tail_note(&tail, line);
            if (!end) break;
            line = end + 1;
        }
        room = (size_t)atoi(argv[4]);
        out = room_new(room);
        cli_fail_msg(out, room, argv[5], atol(argv[3]), &tail);
        if (!room_kept(out, room)) return 4;
        fwrite(out, 1, strlen(out), stdout);
        return 0;
    }
    return 2;
}
"""


def find_compiler(argv):
    if "--compiler" in argv:
        return argv[argv.index("--compiler") + 1]
    return os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")


def as_the_window_shows(text):
    """A line break, a tab or another control character is a space in the window."""
    return "".join(" " if ord(ch) < 0x20 else ch for ch in text)


class SetupHostCliText(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        source = cls.root / "host_text.c"
        source.write_text(HARNESS, encoding="ascii")
        cls.exe = cls.root / ("host_text.exe" if os.name == "nt" else "host_text")
        built = subprocess.run(
            [COMPILER, "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", str(cls.exe), str(source),
             "-I", str(ROOT / "host")],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        if built.returncode != 0:
            raise AssertionError("the host's reader did not compile without a warning:\n" + built.stderr[-3000:])
        cls.count = 0

    # -- the host's side ------------------------------------------------------------------------

    def rows_file(self, rows):
        type(self).count += 1
        path = self.root / ("rows-%d.jsonl" % self.count)
        path.write_bytes(rows if isinstance(rows, bytes) else rows.encode("ascii"))
        return path

    def host(self, *arguments):
        run = subprocess.run([str(self.exe)] + [str(a) for a in arguments], capture_output=True)
        self.assertEqual(run.returncode, 0, "the host's reader ended with %d for %r" % (run.returncode, arguments))
        return run.stdout

    def value(self, rows, key="message", room=4096):
        return self.host("value", self.rows_file(rows), key, room)

    def failure(self, rows, code=3, room=LARGE_ROOM, label="psxrecomp generate"):
        return self.host("fail", self.rows_file(rows), code, room, label)

    # -- the CLI's side -------------------------------------------------------------------------

    def refusal_rows(self):
        """The rows `verify-disc --json-progress` writes for a disc in a Greek
        folder that the SBI rule refuses, and the folder."""
        folder = self.root / ("Παιχνίδια-%d" % self.count)
        type(self).count += 1
        folder.mkdir()
        track = folder / (DISC_NAME + " (Track 1).bin")
        track.write_bytes(bytes(range(256)) * 37)
        cue = folder / (DISC_NAME + ".cue")
        cue.write_text('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % track.name,
                       encoding="utf-8")
        config = folder / "game.toml"
        config.write_text('[game]\nid = "SLES-02529"\n[prepare_disc]\nboot_exe = "SLES_025.29"\n', encoding="utf-8")
        # The rule of the one kit that has one, bound to the made-up track.
        rule = dict(next(iter(dc.REQUIRED_SBI.values())))
        key = (track.stat().st_size, hashlib.sha1(track.read_bytes()).hexdigest())
        rows = io.StringIO()
        arguments = argparse.Namespace(config=str(config), project_root="", disc=str(cue), skip_hash_check=False)
        with patch.dict(dc.REQUIRED_SBI, {key: rule}), \
                patch.object(cli, "activate_embedded_toolchain", lambda *a, **k: False):
            code = cli.cmd_verify_disc(
                arguments, ProgressReporter(json_progress=True, stream=rows, log_stream=io.StringIO()))
        self.assertEqual(code, cli.EXIT_VERIFY)
        return rows.getvalue(), folder

    def player_rows(self):
        """The same rows with the temporary folder replaced by PLAYER_FOLDER, so
        that every length is the same on every machine. The CLI's own writer
        writes them again."""
        rows, folder = self.refusal_rows()
        out = io.StringIO()
        writer = ProgressReporter(json_progress=True, stream=out, log_stream=io.StringIO())
        message = ""
        for line in rows.splitlines():
            row = json.loads(line)
            if "message" not in row:
                continue
            text = row["message"].replace(str(folder.resolve()) + os.sep, PLAYER_FOLDER + "\\")
            if row["event"] == "error":
                self.assertIn(PLAYER_FOLDER, text)
                writer.error(text, code=row["code"], verify_failed=True)
                message = text
            else:
                writer.log(text)
        return out.getvalue(), message

    # -- the tests ------------------------------------------------------------------------------

    def test_a_refusal_that_names_a_greek_file_reaches_the_window_as_the_cli_wrote_it(self):
        rows, folder = self.refusal_rows()
        seen = 0
        for line in rows.splitlines():
            row = json.loads(line)
            if "message" not in row:
                continue
            seen += 1
            self.assertEqual(self.value(line + "\n").decode("utf-8"), as_the_window_shows(row["message"]))
        self.assertGreaterEqual(seen, 2)                      # "Verifying ..." and the refusal
        refusal = json.loads(rows.splitlines()[-1])
        self.assertEqual(refusal["event"], "error")
        named = str((folder / (DISC_NAME + ".sbi")).resolve())
        self.assertIn(named, refusal["message"])              # the CLI names the player's file
        shown = self.failure(rows).decode("utf-8")
        self.assertEqual(shown, "Disc verification failed: " + refusal["message"])
        self.assertIn(named, shown)
        self.assertNotIn("u0394", shown)                      # the escape of a capital delta, without its backslash

    def test_the_refusal_keeps_its_last_sentence(self):
        rows, message = self.player_rows()
        self.assertTrue(message.endswith(LAST_SENTENCE))
        # The fixture means something only while the text fits the host's line
        # and the row's spelling of it does not.
        self.assertLess(len(message.encode("utf-8")), 480)
        self.assertGreater(len(json.dumps(message)) - 2, 480)
        shown = self.failure(rows).decode("utf-8")
        self.assertEqual(shown, "Disc verification failed: " + message)
        self.assertTrue(shown.endswith(LAST_SENTENCE))

    def test_a_text_cut_to_the_room_ends_on_a_whole_letter(self):
        rows, message = self.player_rows()
        whole = "Disc verification failed: " + message
        size = len(whole.encode("utf-8"))
        # The Greek folder names begin here: rooms from there on cut inside them.
        greek = len(whole[:whole.index("Αλέξανδρος")].encode("utf-8"))
        self.assertLess(greek + 20, LAUNCHER_ROOM)
        letters_left_out = 0
        for room in list(range(greek, LAUNCHER_ROOM + 1)) + [size, size + 1]:
            cut = self.failure(rows, room=room)
            text = cut.decode("utf-8")                        # raises on half a letter
            self.assertEqual(text, whole[:len(text)], room)
            self.assertLess(len(cut), room)
            # No more is lost than the letter that did not fit.
            self.assertGreaterEqual(len(cut), min(size, room - 1) - 3, room)
            letters_left_out += len(cut) < min(size, room - 1)
        self.assertGreater(letters_left_out, 10)              # the rooms above did cut inside letters
        self.assertEqual(self.failure(rows, room=size + 1).decode("utf-8"), whole)
        # The progress line of the window: the host reads a message into 240 bytes.
        start = len(message[:message.index("Αλέξανδρος")].encode("utf-8"))
        self.assertLess(start + 20, 240)
        letters_left_out = 0
        for room in range(start, 241):
            cut = self.value(rows.splitlines()[-1] + "\n", room=room)
            text = cut.decode("utf-8")
            self.assertEqual(text, message[:len(text)], room)
            self.assertLess(len(cut), room)
            letters_left_out += len(cut) < room - 1
        self.assertGreater(letters_left_out, 10)

    def test_a_raw_line_cut_to_the_hosts_line_ends_on_a_whole_letter(self):
        # A line that is not a row (a compiler's or a traceback's) is kept as it
        # is, up to the host's line. Here the cut falls inside a two-byte letter.
        raw = "errors: " + "Δ" * 400
        shown = self.failure((raw + "\n").encode("utf-8"), code=1, room=2000)
        text = shown.decode("utf-8")
        self.assertTrue(text.startswith("psxrecomp generate failed (exit 1): errors: ΔΔΔ"))
        self.assertTrue(text.endswith("Δ"))

    def test_every_escape_the_cli_can_write_is_read_back(self):
        for text in ['a "quoted" word, a back\\slash, a /slash',
                     "two lines\nand a\ttab",
                     "café señor über",
                     "日本語のディスク (Disc 1).cue",
                     "a pad 🎮 beyond the first plane",
                     "mixed Δ 日 🎮 é end"]:
            out = io.StringIO()
            ProgressReporter(json_progress=True, stream=out, log_stream=io.StringIO()).log(text)
            row = out.getvalue()
            row.encode("ascii")                               # the CLI's rows are ASCII
            self.assertEqual(self.value(row).decode("utf-8"), as_the_window_shows(text), text)

    def test_a_broken_escape_is_not_read_as_a_letter_and_not_read_past(self):
        cases = [
            (b'{"event":"error","message":"a \\ud83c b"}\n', "a ? b"),          # half a pair
            (b'{"event":"error","message":"a \\udfae b"}\n', "a ? b"),          # the other half
            (b'{"event":"error","message":"a \\ud83c\\u0041 b"}\n', "a ?A b"),  # half a pair, then a letter
        ]
        for row, expected in cases:
            self.assertEqual(self.value(row).decode("utf-8"), expected, row)
        # A row that ends inside an escape: the reader stops at the end.
        for row in (b'{"message":"abc\\u12', b'{"message":"abc\\u', b'{"message":"abc\\'):
            self.assertTrue(self.value(row).decode("utf-8").startswith("abc"), row)

    def test_what_did_not_change(self):
        # A run that wrote no text gives no reason.
        result_only = '{"event":"result","ok":true,"t":0.1}\n'
        self.assertEqual(self.failure(result_only, code=3), b"Disc verification failed (wrong dump).")
        self.assertEqual(self.failure(result_only, code=1), b"psxrecomp generate failed (exit 1).")
        # The last line that looks like an error is the reason, not the last line.
        rows = ('{"event":"log","level":"info","message":"cmake failed: no compiler","t":0.1}\n'
                '{"event":"log","level":"info","message":"cleaning up","t":0.2}\n')
        self.assertEqual(self.failure(rows, code=1, label="psxrecomp rebuild"),
                         b"psxrecomp rebuild failed (exit 1): cmake failed: no compiler")
        # A key that is not in the row, and an empty value.
        for row in ('{"event":"result","ok":true}\n', '{"event":"error","message":""}\n'):
            run = subprocess.run([str(self.exe), "value", str(self.rows_file(row)), "message", "64"],
                                 capture_output=True)
            self.assertEqual(run.returncode, 3, row)


if __name__ == "__main__":
    COMPILER = find_compiler(sys.argv)
    if COMPILER is None:
        print("SKIP: no C compiler; the host's reader was not run")
        sys.exit(77)
    unittest.main(argv=[sys.argv[0]], verbosity=2)
