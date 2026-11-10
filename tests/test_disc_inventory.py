"""Exercise declarative disc inventories with authored bytes, never original media."""
from pathlib import Path
import hashlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import inspect_disc_inventory as inventory


def archive():
    data = bytearray(4096)
    struct.pack_into('<II', data, 0, 1, len(data))
    struct.pack_into('<II', data, 8, 0x20001, 8)
    struct.pack_into('<II', data, 2048, 0x03e00008, 0x27bdfff0)
    return bytes(data)


class Inventory(unittest.TestCase):
    def test_members_and_observed_shapes(self):
        members = inventory.typed_members(archive())
        self.assertEqual([(x['index'], x['kind'], x['offset']) for x in members], [(0, 0x20001, 2048)])
        self.assertEqual(inventory.mips_shapes(members[0]['body']),
                         dict(aligned_jr_ra=1, negative_sp_adjust=1))
        self.assertEqual(inventory.mips_shapes(b'\0' * 7),
                         dict(aligned_jr_ra=0, negative_sp_adjust=0))

    def test_rejects_bad_size_count_bounds_and_alignment(self):
        for offset, value in ((0, 0), (0, 256), (4, 0), (12, 0), (12, 4097), (12, 2049)):
            with self.subTest(offset=offset, value=value):
                data = bytearray(archive())
                struct.pack_into('<I', data, offset, value)
                with self.assertRaises(ValueError): inventory.typed_members(data)
        for alignment in (0, 4, 17):
            with self.assertRaises(ValueError): inventory.typed_members(archive(), alignment)
        with self.assertRaises(ValueError): inventory.typed_members(archive() + b'x')

    def test_inventory_requires_hash_and_complete_classification(self):
        data = archive()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'authored.bin'
            binary.write_bytes(data)
            class Disc:
                files = {'FILE.ARC': (1, len(data))}
                def read(self, name):
                    if name.upper() != 'FILE.ARC': raise KeyError(name)
                    return data
            disc = Disc()
            disc.binary = binary
            profile = dict(schema='psxrecomp disc inventory v1', game_id='AUTHORED',
                           disc_hashes={'sha256': hashlib.sha256(data).hexdigest()},
                           files=[dict(file='FILE.ARC', role='authored member container',
                                       method='typed_aligned_archive', member_count=1)],
                           expected_groups={'2': 1})
            report = inventory.inspect(profile, disc)
            self.assertEqual(report['groups'], {2: 1})
            self.assertEqual(report['files'][0]['members'][0]['sha256'],
                             hashlib.sha256(data[2048:2056]).hexdigest())
            self.assertIn('do not prove code absence', report['limitation'])
            for change in (dict(disc_hashes={'sha256': '0' * 64}), dict(files=[]),
                           dict(files=profile['files'] * 2), dict(expected_groups={'2': 2})):
                with self.subTest(change=change), self.assertRaises(ValueError):
                    inventory.inspect(dict(profile, **change), disc)


if __name__ == '__main__':
    unittest.main()
