#!/usr/bin/env python3
"""Focused regressions for the source-only disc probe."""

from __future__ import annotations

import struct
import unittest

import probe_disc


def directory_record(name: str, extent: int, size: int, *, is_dir: bool) -> bytes:
    encoded = name.encode("ascii")
    length = 33 + len(encoded) + (1 if len(encoded) % 2 == 0 else 0)
    record = bytearray(length)
    record[0] = length
    struct.pack_into("<I", record, 2, extent)
    struct.pack_into(">I", record, 6, extent)
    struct.pack_into("<I", record, 10, size)
    struct.pack_into(">I", record, 14, size)
    record[25] = 0x02 if is_dir else 0
    record[28:32] = b"\x01\x00\x00\x01"
    record[32] = len(encoded)
    record[33 : 33 + len(encoded)] = encoded
    return bytes(record)


class ProbeDiscChdTests(unittest.TestCase):
    """A .chd is probed through libchdr, never rejected as "not a .cue".

    The setup wizard accepts .chd for every disc of a set; update_disc_set
    probes each one. Rejecting .chd left a multi-disc game.toml on its
    placeholder roster (SF2 0.2.0, 2026-09-23). End-to-end digests and
    fingerprints need real media and libchdr; this pins the routing.
    """

    def test_chd_routes_to_libchdr_not_cue_rejection(self) -> None:
        import os
        import tempfile
        from pathlib import Path
        from unittest import mock

        import psx_chd

        with tempfile.TemporaryDirectory() as tmp:
            chd = Path(tmp) / "Game (Disc 2).chd"
            chd.write_bytes(b"MComprHD")
            with mock.patch.dict(os.environ, {"PSXRECOMP_LIBCHDR": ""}), \
                    mock.patch.object(psx_chd, "find_libchdr", return_value=None):
                with self.assertRaises(SystemExit) as ctx:
                    probe_disc.probe(chd)
        message = str(ctx.exception)
        self.assertNotIn("expects a .cue", message)
        self.assertIn(chd.name, message)


class UpdateDiscSetChdReaderTests(unittest.TestCase):
    """update_disc_set builds the CHD reader before probing a .chd set.

    The setup wizard records the disc set BEFORE Generate, and on a fresh kit
    Generate is what first builds libchdr. Without this every .chd set failed
    the probe and game.toml kept its placeholder roster (PS1B-118).
    """

    def _fake_cli(self, result):
        import types
        from unittest import mock

        calls = []
        fake = types.ModuleType("psxrecomp_cli")

        def ensure_chd_reader(project_root, progress):
            calls.append(project_root)
            return result

        fake.ensure_chd_reader = ensure_chd_reader
        return mock.patch.dict("sys.modules", {"psxrecomp_cli": fake}), calls

    def test_missing_reader_is_built_through_the_cli(self) -> None:
        from pathlib import Path
        from unittest import mock

        import psx_chd
        import update_disc_set

        patch_cli, calls = self._fake_cli(Path("libchdr.dll"))
        with patch_cli, mock.patch.object(psx_chd, "find_libchdr", return_value=None):
            self.assertTrue(update_disc_set.ensure_chd_reader(Path("kit")))
        self.assertEqual(calls, [Path("kit")])

    def test_present_reader_is_not_rebuilt(self) -> None:
        from pathlib import Path
        from unittest import mock

        import psx_chd
        import update_disc_set

        patch_cli, calls = self._fake_cli(None)
        with patch_cli, mock.patch.object(psx_chd, "find_libchdr",
                                          return_value=Path("libchdr.dll")):
            self.assertTrue(update_disc_set.ensure_chd_reader(Path("kit")))
        self.assertEqual(calls, [])

    def _run_main(self, reader_ok):
        import sys
        import tempfile
        from pathlib import Path
        from unittest import mock

        import update_disc_set

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            toml = root / "game.toml"
            text = '[game]\ndiscs = [\n    "disc/A (Disc 1).cue",\n    "disc/A (Disc 2).cue",\n]\n'
            toml.write_text(text, encoding="utf-8")
            discs = [root / "A (Disc 1).chd", root / "A (Disc 2).chd"]
            for d in discs:
                d.write_bytes(b"MComprHD")
            argv = ["update_disc_set.py", "--game-toml", str(toml)] + [str(d) for d in discs]
            probe = mock.Mock(side_effect=RuntimeError("probed"))
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(update_disc_set, "ensure_chd_reader",
                                      return_value=reader_ok) as ensure, \
                    mock.patch.object(update_disc_set.probe_disc, "probe", probe):
                rc = update_disc_set.main()
            return rc, ensure, probe, toml.read_text(encoding="utf-8") == text, root

    def test_unbuildable_reader_fails_loudly_and_writes_nothing(self) -> None:
        rc, ensure, probe, unchanged, root = self._run_main(False)
        self.assertEqual(rc, 1)
        ensure.assert_called_once()
        self.assertEqual(ensure.call_args.args[0], root.resolve())
        probe.assert_not_called()
        self.assertTrue(unchanged)

    def test_built_reader_goes_on_to_probe_every_disc(self) -> None:
        rc, ensure, probe, unchanged, _ = self._run_main(True)
        ensure.assert_called_once()
        probe.assert_called_once()      # the fake probe stops after disc 1
        self.assertEqual(rc, 1)
        self.assertTrue(unchanged)


class ProbeDiscPathTests(unittest.TestCase):
    def test_system_cnf_keeps_nested_boot_path(self) -> None:
        cnf = b"BOOT = cdrom:\\TEKKEN3\\SLUS_004.02;1\r\n"
        self.assertEqual(
            probe_disc.parse_system_cnf(cnf),
            r"TEKKEN3\SLUS_004.02",
        )

    def test_resolves_nested_boot_path_case_insensitively(self) -> None:
        root = directory_record("TEKKEN3", 20, probe_disc.USER, is_dir=True)
        nested = directory_record(
            "SLUS_004.02;1", 30, 4096, is_dir=False
        )
        sectors = {20: nested.ljust(probe_disc.USER, b"\0")}

        def read_user(_data: bytes, lba: int) -> bytes:
            return sectors[lba]

        entries = probe_disc.parse_directory_entries(
            root.ljust(probe_disc.USER, b"\0")
        )
        self.assertEqual(
            probe_disc.resolve_iso_file(
                read_user,
                b"",
                entries,
                r"tekken3\slus_004.02",
            ),
            (30, 4096),
        )


if __name__ == "__main__":
    unittest.main()
