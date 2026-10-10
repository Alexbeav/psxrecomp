#!/usr/bin/env python3
"""Check the real emitter and capacity guard using an entirely authored image."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / "runtime/tests/test_kbless_table_capacity.py"
CAPACITY = ROOT / "runtime/include/psx_bios_image.h"
EMITTER = None
EVIDENCE = None
STEM = "AuthoredCapacity"
ROM_BASE = 0xBFC00000
COPY_START, COPY_END, RAM_BASE = 0x100, 0x110, 0x500
FUNCTION_OFFSETS = (0x100, 0x108, 0x200)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, root):
    return subprocess.run(argv, cwd=root, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


class KernelCapacityControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        emitter = EMITTER.resolve(strict=True)
        EVIDENCE.mkdir(parents=True, exist_ok=True)
        cls.root = Path(tempfile.mkdtemp(prefix="kbless-authored-", dir=EVIDENCE))
        # This marker makes the config loader stop here. It cannot resolve a
        # missing fixture path against the checkout's BIOS/seed directories.
        (cls.root / ".gitignore").write_text("", encoding="utf-8")
        rom = bytearray(524288)
        for offset in FUNCTION_OFFSETS:
            struct.pack_into("<II", rom, offset, 0x03E00008, 0)  # jr ra; nop
        image = cls.root / "authored.bin"
        image.write_bytes(rom)
        roots = cls.root / "authored-roots.json"
        roots.write_text(json.dumps({"seeds": [
            {"address": f"0x{ROM_BASE + offset:08X}",
             "label": f"authored_return_{i}", "rationale": "authored jr ra and nop"}
            for i, offset in enumerate(FUNCTION_OFFSETS)
        ]}) + "\n", encoding="utf-8")
        profile = cls.root / "authored.toml"
        profile.write_text(f'''[program]
name = "Authored capacity fixture"
rom = "authored.bin"
load_address = "0x{ROM_BASE:08X}"
entry_pc = "0x{ROM_BASE + FUNCTION_OFFSETS[0]:08X}"
text_size = "0x00080000"

[program.image]
sha256 = "{digest(image)}"
redistributable = true

[recompiler]
seeds = "authored-roots.json"
out_dir = "generated"
out_stem = "{STEM}"
strict = true

[[recompiler.address_model.copy]]
name = "authored two-function window"
rom_lo = "0x{(ROM_BASE & 0x1FFFFFFF) + COPY_START:08X}"
rom_hi = "0x{(ROM_BASE & 0x1FFFFFFF) + COPY_END:08X}"
ram_lo = "0x{RAM_BASE:08X}"
runtime_base = "0x{0x80000000 + RAM_BASE:08X}"
dispatch_key = "ram"
kernel_bless = true
''', encoding="utf-8")
        emitted = run([str(emitter), "--config", str(profile)], cls.root)
        (cls.root / "emitter.stdout.txt").write_text(emitted.stdout, encoding="utf-8")
        (cls.root / "emitter.stderr.txt").write_text(emitted.stderr, encoding="utf-8")
        if emitted.returncode != 0:
            raise AssertionError(f"authored emitter failed: {emitted.returncode}; evidence {cls.root}")
        cls.generated = cls.root / "generated"
        dispatch = cls.generated / f"{STEM}_dispatch.c"
        text = dispatch.read_text(encoding="utf-8")
        count = re.findall(rf"enum\s*\{{\s*{STEM}_psx_bios_kernel_body_count\s*=\s*(\d+)u?\s*\}};", text)
        table = re.findall(rf"static const PsxKernelBody {STEM}_psx_bios_kernel_bodies\[(\d+)\] = \{{\n(.*?)\n\}};", text, re.S)
        if len(count) != 1 or len(table) != 1:
            raise AssertionError("actual emitter omitted an unambiguous kernel body table/count")
        cls.count = int(count[0])
        cls.slots = int(table[0][0])
        cls.rows = [
            tuple(int(value, 16) for value in row)
            for row in re.findall(r"\{\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u\s*\}", table[0][1])
        ]
        cls.expected_rows = [
            (RAM_BASE + offset - COPY_START, RAM_BASE + offset - COPY_START,
             RAM_BASE + offset - COPY_START + 8)
            for offset in FUNCTION_OFFSETS if COPY_START <= offset and offset + 8 <= COPY_END
        ]
        if not cls.rows or cls.count != len(cls.rows) or cls.slots != cls.count:
            raise AssertionError("actual emitter count does not describe nonempty emitted rows")
        headroom = re.findall(r"(?m)^HEADROOM\s*=\s*(\d+)\s*$", GUARD.read_text(encoding="utf-8"))
        if len(headroom) != 1:
            raise AssertionError("capacity guard headroom policy is ambiguous")
        cls.headroom = int(headroom[0])
        receipt = {
            "fixture": "authored zero-filled image with three jr-ra/nop roots",
            "input_recipe_sha256": digest(Path(__file__)),
            "image_sha256": digest(image), "roots_sha256": digest(roots),
            "profile_sha256": digest(profile), "dispatch_sha256": digest(dispatch),
            "emitter_file": str(emitter), "emitter_file_sha256": digest(emitter),
            "guard_sha256": digest(GUARD), "capacity_header_sha256": digest(CAPACITY),
            "emitted_count": cls.count, "emitted_rows": cls.rows,
            "expected_authored_rows": cls.expected_rows,
            "compiled_source_binding": "Requires the external exact-source build/admission record; file hashes alone do not establish it.",
        }
        (cls.root / "provenance.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(f"Authored kernel-capacity evidence: {cls.root}", flush=True)

    def guard(self, header=CAPACITY, generated=None):
        return run([sys.executable, str(GUARD), "--generated",
                    str(self.generated if generated is None else generated),
                    "--capacity-header", str(header)], self.root)

    def header(self, name, capacity):
        path = self.root / name
        path.write_text(f"#define PSX_KBLESS_MAX_ENTRIES {capacity}u\n", encoding="utf-8")
        return path

    def test_emitted_rows_match_authored_geometry(self):
        self.assertEqual(sorted(self.rows), sorted(self.expected_rows))
        self.assertEqual(self.count, len(self.expected_rows))

    def test_actual_capacity_header_accepts_authored_table(self):
        result = self.guard()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"{STEM}: {self.count} rows, capacity ", result.stdout)
        self.assertIn("PASS: 1 profile(s) examined", result.stdout)

    def test_exact_headroom_boundary_passes(self):
        result = self.guard(self.header("boundary.h", self.count + self.headroom))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: 1 profile(s) examined", result.stdout)

    def test_one_row_past_headroom_boundary_fails(self):
        result = self.guard(self.header("headroom-overflow.h", self.count + self.headroom - 1))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"within {self.headroom} of the capacity", result.stdout)
        self.assertIn("FAIL:", result.stdout)

    def test_capacity_overflow_fails(self):
        self.assertGreater(self.count, 1, "overflow control needs two emitted rows")
        cap = self.count - 1
        result = self.guard(self.header("capacity-overflow.h", cap))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"more than the capacity {cap}; the runtime refuses it", result.stdout)
        self.assertIn("FAIL:", result.stdout)

    def test_missing_real_tables_remain_unqualified(self):
        empty = self.root / "no-real-tables"
        empty.mkdir()
        result = self.guard(generated=empty)
        self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
        self.assertIn("SKIP: no generated BIOS tables", result.stdout)
        self.assertNotIn("PASS:", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args, rest = parser.parse_known_args()
    EMITTER, EVIDENCE = args.recompiler, args.evidence_dir
    unittest.main(argv=[__file__, *rest], verbosity=2)
