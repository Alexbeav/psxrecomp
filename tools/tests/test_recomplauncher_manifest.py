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
        # The launcher CRCs the picked .cue/.chd itself, never Redump's tracks.
        self.assertNotIn('dumps', disc)
        self.assertIn('.cue', disc['label'])
        self.assertEqual(m['start']['program'], ['Spyro_the_Dragon_Recompiled.exe',
                                                 'Spyro_the_Dragon_Recompiled'])
        self.assertEqual(m['start']['args'], ['--no-launcher', '--disc', '{input:game}'])
        self.assertEqual(m['saves'], ['{install}/saves'])
        self.assertEqual(m['name'], 'Spyro the Dragon Recompiled')
        self.assertEqual(m['prepare']['program'], m['start']['program'])
        self.assertEqual(m['prepare']['args'], ['--prepare', '--disc', '{input:game}'])
        self.assertEqual(m['prepare']['done'],
                         ['{install}/build-release/Spyro_the_Dragon_Recompiled.exe'])
        lobby = m['settings'][0]
        self.assertEqual(lobby['in'], 'args')
        # An unclicked setting adds nothing, so the first choice must pass nothing.
        self.assertEqual(lobby['choices'][0], {'label': 'No', 'args': []})
        self.assertEqual(lobby['choices'][1]['args'], ['--launcher'])
        for row in m['settings']:
            self.assertEqual(row['choices'][0]['args'], [], row['id'])

    def test_done_names_the_built_game_per_os(self):
        root = self.title(ONE_TRACK, CATALOG)
        m = rm.build_manifest(root, 'X', 'o/x', 'linux')
        self.assertEqual(m['prepare']['done'], ['{install}/build-release/X'])
        with self.assertRaisesRegex(rm.ManifestError, '--os'):
            rm.build_manifest(root, 'X', 'o/x', 'windows-x64')

    def test_video_settings_pass_one_run_switches(self):
        m = rm.build_manifest(self.title(ONE_TRACK), 'X', 'o/x')
        rows = {r['id']: r for r in m['settings']}
        self.assertEqual(set(rows), {'lobby', 'renderer', 'display', 'vsync',
                                     'antialiasing', 'dithering'})
        for row in rows.values():
            self.assertEqual(row['in'], 'args')
        for key in ('renderer', 'display', 'vsync', 'antialiasing', 'dithering'):
            self.assertEqual(rows[key]['choices'][0]['args'], [], key)
            self.assertEqual(rows[key]['group'], 'Video')
        args = [c['args'] for c in rows['vsync']['choices']]
        self.assertIn(['--set', 'video.vsync=vrr'], args)
        self.assertIn(['--renderer', 'vulkan'],
                      [c['args'] for c in rows['renderer']['choices']])

    def test_multi_disc_set(self):
        toml = ONE_TRACK.replace('discs = ["disc/Spyro.cue"]',
                                 'discs = ["disc/a.cue", "disc/b.cue"]').replace(
            'disc_serials = ["SCES-01438"]', 'disc_serials = ["SLUS-1", "SLUS-2"]')
        m = rm.build_manifest(self.title(toml), 'X', 'o/x')
        d1, d2 = m['inputs']
        self.assertEqual((d1['id'], d1['role'], d1['check']), ('disc1', 'disc', {'ids': ['SLUS-1']}))
        self.assertEqual((d2['id'], d2['role'], d2['check']), ('disc2', 'disc', {'ids': ['SLUS-2']}))
        self.assertTrue(d1['label'].startswith('Disc 1') and '.cue' in d1['label'])
        self.assertEqual(m['prepare']['args'], ['--prepare', '--disc', '{input:disc1}',
                                                '--disc', '{input:disc2}'])
        self.assertEqual(m['start']['args'], ['--no-launcher', '--disc', '{input:disc1}'])

    def test_multi_disc_without_per_disc_serials_checks_all(self):
        toml = ONE_TRACK.replace('discs = ["disc/Spyro.cue"]',
                                 'discs = ["disc/a.cue", "disc/b.cue"]')
        m = rm.build_manifest(self.title(toml), 'X', 'o/x')
        self.assertEqual([i['check'] for i in m['inputs']],
                         [{'ids': ['SCES-01438']}, {'ids': ['SCES-01438']}])

    def test_program_set_is_refused(self):
        toml = ONE_TRACK.replace('discs = ["disc/Spyro.cue"]',
                                 'discs = ["disc/a.cue", "disc/b.cue"]\nprogram_discs = [1]')
        root = self.title(toml)
        with self.assertRaisesRegex(rm.Unsupported, 'program_discs'):
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
