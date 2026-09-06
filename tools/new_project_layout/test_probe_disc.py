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
