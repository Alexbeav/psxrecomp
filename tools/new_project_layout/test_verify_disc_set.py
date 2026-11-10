#!/usr/bin/env python3
"""Program identity in verify_disc_set: one program patched per disc is one set.

The executables here are synthetic. Each test builds a PS-X EXE shaped like a
measured retail case, fingerprints it with probe_disc's own page function, and
feeds the probe JSON to verify_disc_set. No disc data is involved.

Measured shapes (pin H13, PS1B-333):
  * Metal Gear Solid (Europe): the two discs' executables differ in one byte of
    a path string;
  * Star Wars: Rebel Assault II (Europe): one byte, the immediate of an
    instruction that loads the disc number;
  * Dragon Warrior VII (USA): 1,024 bytes of header padding and one 3,072-byte
    block that straddles a page boundary, at a load address that is not
    page-aligned.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import probe_disc
import verify_disc_set


def make_exe(*, load: int = 0x80010000, entry: int = 0x80012000,
             text_pages: float = 64, stack: int = 0x801FFFF0, salt: int = 1) -> bytearray:
    """A PS-X EXE with a deterministic, non-repeating body."""
    text_size = int(text_pages * 4096)
    exe = bytearray(probe_disc.EXE_HDR + text_size)
    exe[:8] = b"PS-X EXE"
    struct.pack_into("<I", exe, 0x10, entry)
    struct.pack_into("<I", exe, 0x18, load)
    struct.pack_into("<I", exe, 0x1C, text_size)
    struct.pack_into("<I", exe, 0x30, stack)
    for word in range(text_size // 4):
        struct.pack_into("<I", exe, probe_disc.EXE_HDR + word * 4,
                         (word * 2654435761 + salt * 40503) & 0xFFFFFFFF)
    return exe


def probe_json(exe: bytes, serial: str, number: int, *, pages: bool = True) -> dict:
    """The fields verify_disc_set reads, as probe_disc would report them."""
    pc0, = struct.unpack_from("<I", exe, 0x10)
    t_addr, t_size = struct.unpack_from("<II", exe, 0x18)
    s_addr, = struct.unpack_from("<I", exe, 0x30)
    probe = {
        "cue_path": f"/discs/Game (Disc {number}).cue",
        "cue_name": f"Game (Disc {number}).cue",
        "serial": serial,
        "boot_exe": serial.replace("-", "_")[:8] + "." + serial[-2:],
        "volume_id": f"GAME_DISC{number}",
        "data_track_size": 700000000 + number,
        "data_track_sha1": hashlib.sha1(f"disc {number}".encode()).hexdigest(),
        "required_disc_fp": hashlib.sha256(f"toc {number}".encode()).hexdigest(),
        "track_count": 1,
        "load_address": f"0x{t_addr:08X}",
        "entry_pc": f"0x{pc0:08X}",
        "text_size": f"0x{t_size:08X}",
        "stack_base": f"0x{s_addr:08X}",
        "boot_exe_sha256": hashlib.sha256(exe).hexdigest(),
    }
    if pages:
        probe["boot_exe_size"] = len(exe)
        probe["boot_exe_page_crc32"] = probe_disc.boot_exe_page_crc32(bytes(exe))
    return probe


def run_verify(probes: list[dict]) -> tuple[int, dict, str]:
    """Exit code, disc_set.json content and combined output of verify_disc_set."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        paths = []
        for i, probe in enumerate(probes, start=1):
            path = root / f"disc_probe.{i}.json"
            path.write_text(json.dumps(probe), encoding="utf-8")
            paths.append(str(path))
        out = root / "disc_set.json"
        text = io.StringIO()
        argv = ["verify_disc_set.py", *paths, "--json-out", str(out)]
        with mock.patch.object(sys, "argv", argv), \
                contextlib.redirect_stdout(text), contextlib.redirect_stderr(text):
            code = verify_disc_set.main()
        result = json.loads(out.read_text(encoding="utf-8")) if out.is_file() else {}
    return code, result, text.getvalue()


class PageFingerprintTests(unittest.TestCase):
    def test_one_changed_byte_changes_exactly_one_page(self) -> None:
        exe = make_exe()
        other = bytearray(exe)
        other[probe_disc.EXE_HDR + 5 * 4096 + 0x22] ^= 0x03
        a = probe_disc.boot_exe_page_crc32(bytes(exe))
        b = probe_disc.boot_exe_page_crc32(bytes(other))
        self.assertEqual(len(a), 64)
        self.assertEqual([i for i, (x, y) in enumerate(zip(a, b)) if x != y], [5])

    def test_header_padding_is_not_fingerprinted(self) -> None:
        exe = make_exe()
        other = bytearray(exe)
        other[0x400:0x800] = bytes(range(256)) * 4
        self.assertNotEqual(hashlib.sha256(exe).digest(), hashlib.sha256(other).digest())
        self.assertEqual(probe_disc.boot_exe_page_crc32(bytes(exe)),
                         probe_disc.boot_exe_page_crc32(bytes(other)))

    def test_pages_follow_ram_boundaries_for_an_unaligned_load_address(self) -> None:
        # 0x80017F00 leaves 0x100 bytes in the first RAM page.
        exe = make_exe(load=0x80017F00, entry=0x8008E284, text_pages=4)
        crcs = probe_disc.boot_exe_page_crc32(bytes(exe))
        self.assertEqual(len(crcs), 5)
        other = bytearray(exe)
        other[probe_disc.EXE_HDR + 0x0FF] ^= 0xFF        # last byte of the first page
        other[probe_disc.EXE_HDR + 0x100] ^= 0xFF        # first byte of the second
        changed = probe_disc.boot_exe_page_crc32(bytes(other))
        self.assertEqual([i for i, (x, y) in enumerate(zip(crcs, changed)) if x != y], [0, 1])

    def test_not_an_exe_has_no_fingerprint(self) -> None:
        self.assertEqual(probe_disc.boot_exe_page_crc32(b"\0" * 8192), [])


class ProgramIdentityTests(unittest.TestCase):
    def test_identical_executables_are_one_program(self) -> None:
        exe = make_exe()
        code, result, _ = run_verify([probe_json(exe, "SCUS-94163", 1),
                                      probe_json(exe, "SCUS-94164", 2),
                                      probe_json(exe, "SCUS-94165", 3)])
        self.assertEqual(code, verify_disc_set.EXIT_OK)
        self.assertEqual(result["verdict"], "data-only")
        self.assertEqual(result["program_identity"], "identical")
        self.assertEqual(result["patched_discs"], [])

    def test_one_string_byte_per_disc_is_one_program(self) -> None:
        # Metal Gear Solid (Europe): "...MGS1.EXE" on disc 1, "...MGS2.EXE" on disc 2.
        disc1 = make_exe(entry=0x80066A98, text_pages=88.5)
        offset = probe_disc.EXE_HDR + (0x80060022 - 0x80010000)
        disc1[offset - 18 : offset + 6] = b"cdrom:\\MGS\\MGS1.EXE\0\0\0\0\0"
        disc2 = bytearray(disc1)
        disc2[offset - 18 + 14] = ord("2")
        code, result, text = run_verify([probe_json(disc1, "SLES-01370", 1),
                                         probe_json(disc2, "SLES-11370", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_OK, text)
        self.assertEqual(result["verdict"], "data-only")
        self.assertEqual(result["program_identity"], "patched-per-disc")
        self.assertEqual(result["patched_discs"],
                         [{"index": 2, "pages_differing": 1, "pages_total": 89,
                           "page_addresses": ["0x80060000"]}])
        self.assertIn("one program, patched per disc", text)

    def test_one_instruction_byte_per_disc_is_one_program(self) -> None:
        # Rebel Assault II: `ori v0, zero, 1` on disc 1, `ori v0, zero, 2` on disc 2.
        disc1 = make_exe(entry=0x80090A68, text_pages=169.5)
        offset = probe_disc.EXE_HDR + (0x80032AFC - 0x80010000)
        struct.pack_into("<I", disc1, offset, 0x34020001)
        disc2 = bytearray(disc1)
        struct.pack_into("<I", disc2, offset, 0x34020002)
        code, result, text = run_verify([probe_json(disc1, "SLES-00654", 1),
                                         probe_json(disc2, "SLES-10654", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_OK, text)
        self.assertEqual(result["program_identity"], "patched-per-disc")
        self.assertEqual(result["patched_discs"][0]["page_addresses"], ["0x80032000"])

    def test_padding_blocks_at_an_unaligned_load_address_are_one_program(self) -> None:
        # Dragon Warrior VII: header padding plus a block that straddles a page.
        disc1 = make_exe(load=0x80017F00, entry=0x8008E284, text_pages=164.5)
        disc2 = bytearray(disc1)
        disc2[0x400:0x800] = bytes((i * 7 + 3) & 0xFF for i in range(0x400))
        start = 0x9E000
        disc2[start : start + 0xC00] = bytes((i * 13 + 5) & 0xFF for i in range(0xC00))
        code, result, text = run_verify([probe_json(disc1, "SLUS-01206", 1),
                                         probe_json(disc2, "SLUS-01346", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_OK, text)
        self.assertEqual(result["program_identity"], "patched-per-disc")
        self.assertEqual(result["patched_discs"][0]["pages_differing"], 2)
        self.assertEqual(result["patched_discs"][0]["page_addresses"],
                         ["0x800B5000", "0x800B6000"])

    def test_header_padding_alone_is_one_program(self) -> None:
        disc1 = make_exe()
        disc2 = bytearray(disc1)
        disc2[0x400:0x800] = b"\xA5" * 0x400
        code, result, text = run_verify([probe_json(disc1, "SLUS-00001", 1),
                                         probe_json(disc2, "SLUS-00002", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_OK, text)
        self.assertEqual(result["program_identity"], "patched-per-disc")
        self.assertEqual(result["patched_discs"][0]["pages_differing"], 0)


class DistinctProgramTests(unittest.TestCase):
    def test_different_entry_points_are_two_programs(self) -> None:
        # Resident Evil 2: same size and load address, different program.
        leon = make_exe(entry=0x80078408, text_pages=240.5, salt=1)
        claire = make_exe(entry=0x800783C0, text_pages=240.5, salt=2)
        code, result, text = run_verify([probe_json(leon, "SLUS-00748", 1),
                                         probe_json(claire, "SLUS-00756", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_NEEDS_N_PROGRAMS)
        self.assertEqual(result["verdict"], "n-programs")
        self.assertEqual(result["program_identity"], "distinct")
        self.assertIn("entry_pc", result["differing_program_fields"])
        self.assertEqual(result["patched_discs"], [])
        self.assertIn("each boot their own program", text)

    def test_different_sizes_are_two_programs(self) -> None:
        # Rival Schools: equal entry point and load address, different sizes.
        arcade = make_exe(entry=0x80010008, text_pages=133, salt=1)
        evolution = make_exe(entry=0x80010008, text_pages=137, salt=1)
        code, result, _ = run_verify([probe_json(arcade, "SLUS-00681", 1),
                                      probe_json(evolution, "SLUS-00771", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_NEEDS_N_PROGRAMS)
        self.assertIn("text_size", result["differing_program_fields"])
        self.assertIn("boot_exe_size", result["differing_program_fields"])

    def test_same_shape_with_most_pages_different_is_two_programs(self) -> None:
        one = make_exe(salt=1)
        two = make_exe(salt=2)
        code, result, text = run_verify([probe_json(one, "SLUS-00001", 1),
                                         probe_json(two, "SLUS-00002", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_NEEDS_N_PROGRAMS)
        self.assertEqual(result["program_identity"], "distinct")
        self.assertIn("64 of 64 pages differ", text)

    def test_one_page_over_the_bound_is_two_programs(self) -> None:
        one = make_exe(text_pages=64)        # bound: 64 // 16 = 4 pages
        ok, over = bytearray(one), bytearray(one)
        for page in range(4):
            ok[probe_disc.EXE_HDR + page * 4096] ^= 0xFF
        for page in range(5):
            over[probe_disc.EXE_HDR + page * 4096] ^= 0xFF
        self.assertEqual(verify_disc_set.patched_page_limit(64), 4)
        code, _, _ = run_verify([probe_json(one, "SLUS-00001", 1), probe_json(ok, "SLUS-00002", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_OK)
        code, _, text = run_verify([probe_json(one, "SLUS-00001", 1), probe_json(over, "SLUS-00002", 2)])
        self.assertEqual(code, verify_disc_set.EXIT_NEEDS_N_PROGRAMS)
        self.assertIn("5 of 64 pages differ", text)

    def test_the_bound_never_exceeds_eight_pages_and_never_drops_below_one(self) -> None:
        self.assertEqual(verify_disc_set.patched_page_limit(19), 1)
        self.assertEqual(verify_disc_set.patched_page_limit(89), 5)
        self.assertEqual(verify_disc_set.patched_page_limit(500), 8)

    def test_a_probe_without_page_fingerprints_is_refused_not_guessed(self) -> None:
        # An older probe carries only the whole-file hash. A differing hash then
        # cannot be told from another program, so the set is refused as before.
        disc1 = make_exe()
        disc2 = bytearray(disc1)
        disc2[probe_disc.EXE_HDR + 0x1234] ^= 0x01
        code, result, text = run_verify([probe_json(disc1, "SLES-01370", 1, pages=False),
                                         probe_json(disc2, "SLES-11370", 2, pages=False)])
        self.assertEqual(code, verify_disc_set.EXIT_NEEDS_N_PROGRAMS)
        self.assertEqual(result["program_identity"], "distinct")
        self.assertIn("no page fingerprints", text)

    def test_identical_executables_need_no_page_fingerprints(self) -> None:
        exe = make_exe()
        code, result, _ = run_verify([probe_json(exe, "SCUS-94163", 1, pages=False),
                                      probe_json(exe, "SCUS-94164", 2, pages=False)])
        self.assertEqual(code, verify_disc_set.EXIT_OK)
        self.assertEqual(result["program_identity"], "identical")


if __name__ == "__main__":
    unittest.main()
