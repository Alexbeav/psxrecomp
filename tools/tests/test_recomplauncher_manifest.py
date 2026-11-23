"""tools/recomplauncher_manifest.py: the manifest is written from the title's own records.

Hermetic: each case writes a small title folder (game.toml, catalog_identity.json)
in a temporary directory. The schema check is tried only when `jsonschema` and a
schema file are present (RECOMPLAUNCHER_SCHEMA names the file); otherwise it skips
with a line that says so.
"""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import recomplauncher_manifest as rm  # noqa: E402

ONE_TRACK = '''
[game]
name = "Spyro the Dragon"
id = "SCES-01438"
discs = ["disc/Spyro.cue"]
disc_serials = ["SCES-01438"]

[prepare_disc]
known_sizes = [732391632]
known_crc32 = ["ADB35999"]

[runtime]
window_title = "Spyro the Dragon Recompiled"
memcard_dir = "saves"

[netplay]
required_tracks = 1
'''

CATALOG = {'marketing': {'region': 'Europe'}, 'rom_identity': {'track_counts': [1]}}


class ManifestTest(unittest.TestCase):
    def title(self, game_toml, catalog=None):
        root = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(root, ignore_errors=True))
        (root / 'game.toml').write_text(game_toml, encoding='utf-8')
        if catalog is not None:
            (root / 'catalog_identity.json').write_text(json.dumps(catalog), encoding='utf-8')
        return root

    def test_one_track_disc(self):
        m = rm.build_manifest(self.title(ONE_TRACK, CATALOG),
                              'Spyro_the_Dragon_Recompiled', 'o/spyro')
        self.assertEqual(m['format'], 1)
        disc, = m['inputs']
        self.assertEqual(disc['id'], 'game')
        self.assertEqual(disc['check'], {'ids': ['SCES-01438']})
        self.assertEqual(disc['dumps'], [{'name': 'Europe', 'crc32': 'adb35999',
                                          'size': 732391632}])
        self.assertEqual(m['start']['program'], ['Spyro_the_Dragon_Recompiled.exe',
                                                 'Spyro_the_Dragon_Recompiled'])
        self.assertEqual(m['start']['args'], ['--disc', '{input:game}'])
        self.assertEqual(m['saves'], ['{install}/saves'])
        self.assertEqual(m['name'], 'Spyro the Dragon Recompiled')

    def test_multi_track_disc_has_no_dumps(self):
        toml = ONE_TRACK.replace('required_tracks = 1', 'required_tracks = 3')
        m = rm.build_manifest(self.title(toml, {'rom_identity': {'track_counts': [3]}}),
                              'X', 'o/x')
        self.assertNotIn('dumps', m['inputs'][0])

    def test_multi_disc_is_refused(self):
        toml = ONE_TRACK.replace('discs = ["disc/Spyro.cue"]',
                                 'discs = ["disc/a.cue", "disc/b.cue"]')
        root = self.title(toml)
        with self.assertRaisesRegex(rm.Unsupported, 'multi-disc'):
            rm.build_manifest(root, 'X', 'o/x')
        out = root / 'out.json'
        self.assertEqual(rm.main(['write', '--root', str(root), '--exe-name', 'X',
                                  '--repo', 'o/x', '--out', str(out)]), rm.EXIT_UNSUPPORTED)
        self.assertFalse(out.exists())

    def test_no_serial_is_refused(self):
        toml = ONE_TRACK.replace('id = "SCES-01438"\n', '').replace(
            'disc_serials = ["SCES-01438"]\n', '')
        with self.assertRaisesRegex(rm.ManifestError, 'disc_serials'):
            rm.build_manifest(self.title(toml), 'X', 'o/x')

    def test_exe_name_without_extension(self):
        with self.assertRaisesRegex(rm.ManifestError, 'without .exe'):
            rm.build_manifest(self.title(ONE_TRACK), 'X.exe', 'o/x')

    def test_write_then_schema_check(self):
        out = Path(tempfile.mkdtemp()) / 'recomplauncher.json'
        self.addCleanup(lambda: __import__('shutil').rmtree(out.parent, ignore_errors=True))
        root = self.title(ONE_TRACK, CATALOG)
        self.assertEqual(rm.main(['write', '--root', str(root), '--exe-name', 'X',
                                  '--repo', 'o/x', '--out', str(out)]), 0)
        self.assertEqual(json.loads(out.read_text(encoding='utf-8'))['format'], 1)
        schema = os.environ.get('RECOMPLAUNCHER_SCHEMA')
        try:
            import jsonschema  # noqa: F401
        except ModuleNotFoundError:
            self.skipTest('jsonschema not installed; schema check not run')
        if not schema or not Path(schema).is_file():
            self.skipTest('RECOMPLAUNCHER_SCHEMA not set; schema check not run')
        self.assertEqual(rm.main(['check', '--manifest', str(out), '--schema', schema]), 0)


if __name__ == '__main__':
    unittest.main()
