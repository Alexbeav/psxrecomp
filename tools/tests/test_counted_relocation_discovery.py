"""Unpositioned module discovery must report facts without inventing a base."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aot_overlay_spike.extract_generic import counted_relocation_inventory


class CountedDiscoveryTest(unittest.TestCase):
    def test_complete_container_reports_members_and_requires_placement(self):
        image = struct.pack('<4I', 8, 0x0c000002, 0x03e00008, 0)
        original = struct.pack('<4I', 1, 0, 16, 0) + image + struct.pack('<3I', 0, 7, 0xffffffff)
        inventory = counted_relocation_inventory(original)
        self.assertTrue(inventory['placement_required'])
        self.assertNotIn('load_addr', inventory)
        self.assertEqual(inventory['members'][0]['image_offset'], 16)
        self.assertEqual(inventory['members'][0]['relocation_count'], 2)
        self.assertEqual(inventory['members'][0]['image_size'], 16)

    def test_partial_stream_and_pointer_header_are_not_relocation_containers(self):
        for data in (struct.pack('<4I', 1, 0x80100000, 0x03e00008, 0),
                     struct.pack('<4I', 1, 0, 4, 0) + struct.pack('<2I', 0, 0),
                     b'PS-X EXE' + bytes(2040)):
            with self.subTest(data=data[:16]):
                self.assertIsNone(counted_relocation_inventory(data))


if __name__ == '__main__':
    unittest.main()
