"""Authored controller cases: no retail inputs, BIOS, media or saved state."""
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

import dualshock_route as route


class ControllerExport(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def movie(self, rows=None, changes=None):
        if rows is None:
            rows = ['|    0,....|  128,  128,  128,  128,.................|']
        members = {name: b'' for name in route.MEMBERS}
        members.update({'BizState 1.0': b'2\r\n', 'BizVersion.txt': b'Version 2.9.1\r\n',
                        'Header.txt': b'Core Nymashock\nPlatform PSX\nemuVersion Version 2.9.1\n',
                        'SyncSettings.json': b'{}',
                        'Input Log.txt': ('\n'.join(['[Input]', route.LOGKEY, *rows, '[/Input]']) + '\n').encode()})
        members.update(changes or {})
        movie = self.root / 'authored.bk2'
        with zipfile.ZipFile(movie, 'w') as archive:
            for name, value in members.items():
                archive.writestr(name, value)
        return movie

    def test_all_buttons_axes_and_physical_analog(self):
        rows = []
        for index in range(17):
            buttons = ['.'] * 17
            buttons[index] = 'P'
            rows.append('|    0,....|    1,    0,  128,  255,' + ''.join(buttons) + '|')
        states = route.read_movie(self.movie(rows))
        # Independently stated protocol order, including L3/R3 and Analog.
        bits = [4, 6, 7, 5, 0, 3, 12, 14, 15, 13, 10, 8, 11, 9, 1, 2]
        self.assertEqual(states, [(65535 ^ (1 << bit), 1, 0, 128, 255, 0) for bit in bits]
                         + [(65535, 1, 0, 128, 255, 1)])

    def test_wire_identity_sequence_and_axis_only_steps(self):
        rows = [(65535, 128, 128, 128, 128, 0),
                (65535, 1, 128, 128, 128, 0),
                (65535, 1, 128, 128, 128, 0),
                (65535, 1, 128, 128, 128, 1)]
        output = self.root / 'route.bin'
        receipt = route.write_route(rows, output)
        self.assertEqual(receipt['steps'], 3)
        wire = output.read_bytes()
        self.assertEqual(struct.unpack('<8sIIII', wire[:24]), (b'PSXRTI2\0', 2, 12, 4, 0))
        for i, row in enumerate(rows):
            self.assertEqual(struct.unpack_from('<IH6B', wire, 24 + i * 12), (i + 1, *row, 0))
        with self.assertRaises(FileExistsError):
            route.write_route(rows, output)
        self.assertEqual(output.read_bytes(), wire)

    def test_unsupported_source_payloads_and_layouts(self):
        invalid = [
            {'Core.bin': b'state'}, {'BizState 1.0': b'1\r\n'},
            {'BizVersion.txt': b'Version 2.3\r\n'},
            {'Header.txt': b'Core Octoshock\nPlatform PSX\nemuVersion Version 2.9.1\n'},
            {'Header.txt': b'Core Nymashock\nPlatform PSX\nemuVersion Version 2.9.1\nStartsFromSavestate True\n'},
            {'Input Log.txt': b'[Input]\nwrong logkey\n[/Input]\n'},
        ]
        for changes in invalid:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                route.read_movie(self.movie(changes=changes))

    def test_bad_events_axes_records_and_missing_inputs(self):
        for rows in [[], ['broken record'],
                     ['|    1,....|128,128,128,128,.................|'],
                     ['|    0,P...|128,128,128,128,.................|'],
                     ['|    0,....|-1,128,128,128,.................|'],
                     ['|    0,....|256,128,128,128,.................|'],
                     ['|    0,....|128,128,128,128,................|'],
                     ['|    0,....|128,128,128,128,.................|extra|']]:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                route.read_movie(self.movie(rows))

    def test_validation_precedes_file_creation(self):
        valid = (65535, 128, 128, 128, 128, 0)
        invalid = [[], [valid, (65535, 128, 128, 128, 128, 2)],
                   [valid, (65536, 128, 128, 128, 128, 0)],
                   [valid, (65535, 128, 128, -1, 128, 0)],
                   [valid, (65535, 128, 128, 128, 128, False)],
                   [valid] * (route.MAX_FRAMES + 1),
                   [(65535, i % 2, 128, 128, 128, 0) for i in range(route.MAX_STEPS + 1)]]
        for rows in invalid:
            output = self.root / 'must-not-exist.bin'
            with self.assertRaises(ValueError):
                route.write_route(rows, output)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
