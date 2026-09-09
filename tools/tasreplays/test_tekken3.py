"""Asset-free tests for intake failures and replay verification."""
import gzip
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import tekken3


class RecipeTests(unittest.TestCase):
    def test_renamed_tracks_keep_qualified_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payloads = [b'data', b'audio-one', b'audio-two']
            names = ['renamed data.bin', 'music 2.bin', 'music 3.bin']
            for name, data in zip(names, payloads):
                (root / name).write_bytes(data)
            cue = root / 'game.cue'
            cue.write_text('REM an informational comment\n'
                'FILE "renamed data.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n'
                'FILE "music 2.bin" BINARY\n TRACK 02 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:02:00\n'
                'FILE "music 3.bin" BINARY\n TRACK 03 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:02:00\n')
            identities = [(len(data), hashlib.sha256(data).hexdigest()) for data in payloads]
            with patch.object(tekken3, 'TRACKS', identities):
                self.assertEqual(tekken3.verify_cue(cue), [root / name for name in names])
                (root / names[2]).write_bytes(b'wrong-two')
                with self.assertRaisesRegex(ValueError, 'Wrong or missing'):
                    tekken3.verify_cue(cue)

    def test_changed_pregap_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for number in (1, 2, 3):
                (root / f'{number}.bin').write_bytes(b'x')
            cue = root / 'game.cue'
            cue.write_text('FILE "1.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n'
                'FILE "2.bin" BINARY\n TRACK 02 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:00:00\n'
                'FILE "3.bin" BINARY\n TRACK 03 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:02:00\n')
            with self.assertRaisesRegex(ValueError, 'original audio pregaps'):
                tekken3.verify_cue(cue)

    def test_any_page_or_clock_difference_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = ['1', '12345'] + ['0000000000000000'] * 512
            expected = hashlib.sha256(' '.join(original).encode()).hexdigest()
            (root / 'tekken3-reference.tsv.gz').write_bytes(gzip.compress(f'1\t{expected}\n'.encode()))
            for changed_index in (1, 2, 513):
                modified = original.copy()
                modified[changed_index] = '12346' if changed_index == 1 else '0000000000000001'
                (root / 'ram-pages.tsv').write_text('\t'.join(modified) + '\n')
                with patch.object(tekken3, 'HERE', root):
                    with self.assertRaisesRegex(ValueError, 'diverged at return 1'):
                        tekken3.compare_replay(root)

    def test_missing_returns_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'ram-pages.tsv').write_text('')
            with self.assertRaisesRegex(ValueError, 'Incomplete replay'):
                tekken3.compare_replay(root)


if __name__ == '__main__':
    unittest.main()
