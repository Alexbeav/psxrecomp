"""Authored 2.8 controller cases: no retail inputs, BIOS, media or saved state."""
from pathlib import Path
import tempfile
import unittest
import zipfile

import dualshock_route as route
import nymashock28_movie as relayout

HEADER_28 = (b'MovieVersion BizHawk v2.0\r\nAuthor authored\r\nemuVersion Version 2.8\r\n'
             b'OriginalEmuVersion Version 2.8\r\nPlatform PSX\r\nGameName authored\r\n'
             b'Core Nymashock\r\n\r\n')
SYNC_28 = (b'{"o":{"$type":"BizHawk.Emulation.Cores.Waterbox.NymaCore+NymaSyncSettings, '
           b'BizHawk.Emulation.Cores","MednafenValues":{},"PortDevices":{}}}\r\n')
NEUTRAL = '|....|32768,32768,32768,32768,.................|'
MIXED = ['|....|65280,    0,  256,32768,U..R.S.X.........|',
         NEUTRAL,
         '|....|32768,32768,32768,32768,..D......O..1..lA|',
         '|....|32768,32768,32768,32768,..D......O..1..lA|']
MIXED_291 = ['|    0,....|  128,    0,    1,  255,U..R.S.X.........|',
             '|    0,....|  128,  128,  128,  128,.................|',
             '|    0,....|  128,  128,  128,  128,..D......O..1..lA|',
             '|    0,....|  128,  128,  128,  128,..D......O..1..lA|']


class Relayout(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def movie(self, rows=None, changes=None, name='authored28.bk2'):
        if rows is None:
            rows = list(MIXED)
        members = {'Header.txt': HEADER_28, 'Comments.txt': b'\r\n', 'Subtitles.txt': b'\r\n',
                   'SyncSettings.json': SYNC_28,
                   'Input Log.txt': ('\r\n'.join(['[Input]', route.LOGKEY_28, *rows, '[/Input]']) + '\r\n').encode()}
        members.update(changes or {})
        movie = self.root / name
        with zipfile.ZipFile(movie, 'w') as archive:
            for name, value in members.items():
                archive.writestr(name, value)
        return movie

    def test_relayout_members_logkey_and_rows(self):
        movie = self.movie()
        output = self.root / 'authored291.bk2'
        receipt = relayout.relayout(movie, output)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.namelist(), list(relayout.MEMBER_ORDER))
            self.assertEqual(archive.read('BizState 1.0'), b'2\r\n')
            self.assertEqual(archive.read('BizVersion.txt'), b'Version 2.9.1\r\n')
            self.assertEqual(archive.read('Header.txt'), HEADER_28)
            self.assertEqual(archive.read('Comments.txt'), b'\r\n')
            self.assertEqual(archive.read('Subtitles.txt'), b'\r\n')
            self.assertEqual(archive.read('SyncSettings.json'), SYNC_28)
            self.assertEqual(archive.read('Input Log.txt'),
                             ('\r\n'.join(['[Input]', route.LOGKEY, *MIXED_291, '[/Input]']) + '\r\n').encode())
            self.assertEqual({info.date_time for info in archive.infolist()}, {relayout.ZIP_DATE})
        self.assertEqual(receipt['schema'], 'nymashock28-relayout-v1')
        self.assertEqual((receipt['rows'], receipt['console_events'], receipt['lossless']), (4, 0, True))
        self.assertEqual(receipt['header_emu_version'], 'Version 2.8')
        pressed = {name: count for name, count in receipt['buttons'].items() if count}
        self.assertEqual(pressed, {'P1 D-Pad Up': 1, 'P1 D-Pad Right': 1, 'P1 Start': 1, 'P1 X': 1,
                                   'P1 D-Pad Left': 2, 'P1 ○': 2, 'P1 R1': 2, 'P1 Right Stick, Button': 2,
                                   'P1 Analog': 2})
        self.assertEqual(len(receipt['buttons']), 17)
        self.assertEqual(len(relayout.BUTTON_NAMES), 17)

    def test_deterministic_and_never_overwrites(self):
        movie = self.movie()
        first, second = self.root / 'first.bk2', self.root / 'second.bk2'
        a = relayout.relayout(movie, first)
        b = relayout.relayout(movie, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(a['output_sha256'], b['output_sha256'])
        wire = first.read_bytes()
        with self.assertRaises(FileExistsError):
            relayout.relayout(movie, first)
        self.assertEqual(first.read_bytes(), wire)

    def test_verify_and_read_movie_28_agree(self):
        movie = self.movie()
        output = self.root / 'authored291.bk2'
        relayout.relayout(movie, output)
        self.assertEqual(relayout.verify(movie, output), {'rows': 4, 'verified': True})
        rows = route.read_movie_28(movie)
        self.assertEqual(rows, route.read_movie(output, emu_version='Version 2.8'))
        # Independently stated protocol bits: Up 4, Right 5, Start 3, X 14; Left 7, O 13, R1 11, R3 2.
        self.assertEqual(rows, [(65535 ^ (1 << 4) ^ (1 << 5) ^ (1 << 3) ^ (1 << 14), 128, 0, 1, 255, 0),
                                (65535, 128, 128, 128, 128, 0),
                                (65535 ^ (1 << 7) ^ (1 << 13) ^ (1 << 11) ^ (1 << 2), 128, 128, 128, 128, 1),
                                (65535 ^ (1 << 7) ^ (1 << 13) ^ (1 << 11) ^ (1 << 2), 128, 128, 128, 128, 1)])
        # Provenance is kept, so the default 2.9.1-declared reader still refuses the header.
        with self.assertRaises(ValueError):
            route.read_movie(output)
        # A copied member that drifts is caught even though the rows still agree.
        drifted = self.movie(changes={'Comments.txt': b'drift\r\n'}, name='drifted28.bk2')
        with self.assertRaises(ValueError):
            relayout.verify(drifted, output)
        with self.assertRaises(ValueError):
            relayout.verify(movie, self.movie(rows=MIXED[:3], name='short28.bk2'))

    def test_rejected_sources_create_nothing(self):
        other_port = NEUTRAL[:-1] + '.................|'
        invalid = [
            {'rows': ['|P...|32768,32768,32768,32768,.................|']},
            {'rows': ['|....|32769,32768,32768,32768,.................|']},
            {'rows': ['|....|32768,32768,32768,  255,.................|']},
            {'rows': ['|....|65536,32768,32768,32768,.................|']},
            {'rows': ['|....|-256,32768,32768,32768,.................|']},
            {'rows': [other_port]},
            {'rows': ['|....|32768,32768,32768,32768,................|']},
            {'rows': ['|....|32768,32768,32768,.................|']},
            {'rows': ['|    0,....|  128,  128,  128,  128,.................|']},
            {'rows': []},
            {'changes': {'BizVersion.txt': b'Version 2.9.1\r\n'}},
            {'changes': {'Header.txt': HEADER_28.replace(b'Version 2.8', b'Version 2.9.1')}},
            {'changes': {'Header.txt': HEADER_28.replace(b'Core Nymashock', b'Core Octoshock')}},
            {'changes': {'Header.txt': HEADER_28 + b'StartsFromSavestate True\r\n'}},
            {'changes': {'Header.txt': HEADER_28 + b'StartsFromSaveRam False\r\n'}},
            {'changes': {'SyncSettings.json': SYNC_28.replace(b'"PortDevices":{}', b'"PortDevices":{"1":"none"}')}},
            {'changes': {'Input Log.txt': ('\r\n'.join(['[Input]', route.LOGKEY, *MIXED_291, '[/Input]']) + '\r\n').encode()}},
        ]
        for index, case in enumerate(invalid):
            movie = self.movie(case.get('rows'), case.get('changes'), name='invalid%d.bk2' % index)
            output = self.root / ('must-not-exist%d.bk2' % index)
            with self.subTest(case=case):
                with self.assertRaises(ValueError):
                    relayout.relayout(movie, output)
                self.assertFalse(output.exists())
                with self.assertRaises(ValueError):
                    route.read_movie_28(movie)


if __name__ == '__main__':
    if not unittest.main(exit=False).result.wasSuccessful():
        raise SystemExit(1)
    print('Nymashock 2.8 relayout: exact 2.9.1 rows/members, determinism, verify agreement and refusals pass')
