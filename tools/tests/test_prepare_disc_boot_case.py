#!/usr/bin/env python3
"""prepare_disc must find the boot program regardless of the case SYSTEM.CNF uses.

ISO 9660 root records are upper case (SLUS_006.63); Bushido Blade 2's
SYSTEM.CNF says ``BOOT = cdrom:\\slus_006.63;1`` and the probe keeps that
spelling in game.toml. A case-sensitive lookup made the public kit's first-run
setup stop with "missing slus_006.63 on disc". The staged file keeps the
requested spelling so game.toml's exe path stays valid on Linux too.
"""
import importlib.util
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))  # prepare_disc imports disc_companion by module name
spec = importlib.util.spec_from_file_location("prepare_disc_under_test", ROOT / "tools" / "prepare_disc.py")
pd = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = pd
spec.loader.exec_module(pd)

USER = 2048


def record(name: bytes, extent: int, size: int) -> bytes:
    body = bytearray(33 + len(name))
    body[0] = len(body) + (len(body) & 1)
    struct.pack_into("<I", body, 2, extent)
    struct.pack_into("<I", body, 10, size)
    body[32] = len(name)
    body[33:33 + len(name)] = name
    if len(body) & 1:
        body += b"\x00"
    return bytes(body)


def synthetic_disc(boot_record_name: bytes) -> dict[int, bytes]:
    """Sectors by LBA: PVD at 16, root directory at 20, files at 21 and 22."""
    exe = b"PS-X EXE" + b"\x00" * 100
    cnf = b"BOOT = cdrom:\\slus_006.63;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801fff00\r\n"
    root = record(b"\x00", 20, USER) + record(b"\x01", 20, USER) + record(b"SYSTEM.CNF;1", 21, len(cnf)) + record(boot_record_name, 22, len(exe))
    pvd = bytearray(USER)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    struct.pack_into("<I", pvd, 158, 20)
    struct.pack_into("<I", pvd, 166, USER)
    return {16: bytes(pvd), 20: root.ljust(USER, b"\x00"), 21: cnf.ljust(USER, b"\x00"), 22: exe.ljust(USER, b"\x00")}


class BootCaseTests(unittest.TestCase):
    def test_lower_case_boot_name_resolves_to_the_upper_case_record(self):
        sectors = synthetic_disc(b"SLUS_006.63;1")
        entries, files = pd.extract_via(lambda data, lba: sectors[lba], b"", "slus_006.63")
        self.assertIn("SLUS_006.63", entries)
        self.assertIn("SYSTEM.CNF", files)
        # staged under the spelling game.toml uses, with the PS-X EXE bytes
        self.assertIn("slus_006.63", files)
        self.assertTrue(files["slus_006.63"].startswith(b"PS-X EXE"))

    def test_exact_case_still_works_and_missing_still_fails(self):
        sectors = synthetic_disc(b"SLUS_006.63;1")
        entries, files = pd.extract_via(lambda data, lba: sectors[lba], b"", "SLUS_006.63")
        self.assertTrue(files["SLUS_006.63"].startswith(b"PS-X EXE"))
        with self.assertRaises(SystemExit) as ctx:
            pd.extract_via(lambda data, lba: sectors[lba], b"", "SLUS_999.99")
        self.assertIn("missing SLUS_999.99 on disc", str(ctx.exception))

    def test_resolve_root_entry(self):
        entries = {"SYSTEM.CNF": (1, 2), "SLUS_006.63": (3, 4)}
        self.assertEqual(pd.resolve_root_entry(entries, "slus_006.63"), "SLUS_006.63")
        self.assertEqual(pd.resolve_root_entry(entries, "SLUS_006.63"), "SLUS_006.63")
        self.assertIsNone(pd.resolve_root_entry(entries, "nope"))


if __name__ == "__main__":
    unittest.main()
