"""Exercise the real runner's executable staging with authored files only."""
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_native as runner


class LegacyClockSelectionTests(unittest.TestCase):
    def test_explicit_pair_preserves_legacy_path_bytes(self):
        old = r'I:\old lane\random.psxrng'
        sha = 'a' * 64
        self.assertEqual(runner.legacy_clock_resume_environment(old, sha, Path('native.pst'), True, {'sha256': sha}), {
            'PSX_TAS_RESUME_LEGACY_CLOCK_PATH': old,
            'PSX_TAS_RESUME_LEGACY_CLOCK_SHA256': sha})

    def test_ordinary_run_selects_no_legacy_environment(self):
        self.assertEqual(runner.legacy_clock_resume_environment(None, None, None, False, None), {})

    def test_incomplete_or_incompatible_selection_is_rejected(self):
        sha = 'a' * 64
        cases = [('old', None, Path('native.pst'), True, {'sha256': sha}),
                 (None, sha, Path('native.pst'), True, {'sha256': sha}),
                 ('old', sha, None, True, {'sha256': sha}),
                 ('old', sha, Path('native.pst'), False, {'sha256': sha}),
                 ('old', sha, Path('native.pst'), True, None),
                 ('old', 'b' * 64, Path('native.pst'), True, {'sha256': sha}),
                 ('old', 'not-a-hash', Path('native.pst'), True, {'sha256': sha}),
                 ('', sha, Path('native.pst'), True, {'sha256': sha})]
        for args in cases:
            with self.subTest(args=args), self.assertRaises(ValueError):
                runner.legacy_clock_resume_environment(*args)


class StagingTests(unittest.TestCase):
    def run_fixture(self, root, mode):
        exe = root / ('fixture.cmd' if os.name == 'nt' else 'fixture.sh')
        exe.write_bytes(b'@echo off\r\nexit /b 0\r\n' if os.name == 'nt' else b'#!/bin/sh\nexit 0\n')
        if os.name != 'nt':
            exe.chmod(mode)
        for name in ('game.toml', 'disc.fixture', 'bios.fixture'):
            (root / name).write_bytes(b'authored fixture')
        route = root / 'input.psxrti'
        route.write_bytes(struct.pack('<8sIIII', b'PSXRTI1\0', 1, 8, 2, 0)
                          + struct.pack('<IHHIHH', 1, 0xffff, 0, 2, 0xffff, 0))
        run = root / 'run with spaces'
        sha = hashlib.sha256(exe.read_bytes()).hexdigest()
        result = subprocess.run(
            [sys.executable, str(HERE / 'run_native.py'), str(run), '--exe', str(exe),
             '--game', str(root / 'game.toml'), '--route', str(route),
             '--disc', str(root / 'disc.fixture'), '--bios', str(root / 'bios.fixture'),
             '--expected-exe-sha256', sha, '--timeout', '5'],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        staged = run / exe.name
        self.assertEqual(staged.read_bytes(), exe.read_bytes())
        manifest = json.loads((run / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['staged_executable']['sha256'], sha)
        self.assertEqual(manifest['expected_exe_sha256'], sha)
        if os.name != 'nt':
            # Include actual launch failure in the old-code assertion, not just mode.
            self.assertEqual(stat.S_IMODE(staged.stat().st_mode), mode, result.stderr)
        return run, result

    def test_executable_is_staged_and_really_launched(self):
        with tempfile.TemporaryDirectory(prefix='native staging ') as directory:
            run, result = self.run_fixture(Path(directory), 0o755)
            self.assertTrue((run / 'exit.json').exists(), result.stderr)
            exited = json.loads((run / 'exit.json').read_text(encoding='utf-8'))
            self.assertEqual(exited['exit_code'], 0)
            self.assertFalse(exited['timed_out'])
            self.assertIsNone(exited['stop_reason'])
            self.assertTrue((run / 'process.json').exists())
            # The stand-in exits without consuming the route or claiming gameplay.
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(json.loads(result.stdout.splitlines()[-1])['input_playback_complete'])

    @unittest.skipIf(os.name == 'nt', 'Windows does not use Unix execute permission bits')
    def test_nonexecutable_source_is_not_made_executable(self):
        with tempfile.TemporaryDirectory(prefix='native staging restricted ') as directory:
            run, result = self.run_fixture(Path(directory), 0o640)
            self.assertIn('PermissionError', result.stderr)
            self.assertFalse((run / 'process.json').exists())
            self.assertFalse((run / 'exit.json').exists())


class SpuKeyoffModelTests(unittest.TestCase):
    def test_explicit_model_and_compatible_transition(self):
        source = 'octoshock-2.2.2-bounded-quad'
        self.assertEqual(runner.spu_keyoff_resume_environment('default', None, None, False, source), {})
        self.assertEqual(runner.spu_keyoff_resume_environment('octoshock-1.11.9', None, None, False, source),
                         {'PSX_SPU_KEYOFF_MODEL': 'octoshock-1.11.9'})
        self.assertEqual(runner.spu_keyoff_resume_environment('octoshock-1.11.9', 'unversioned', Path('native.pst'), True, source),
                         {'PSX_SPU_KEYOFF_MODEL': 'octoshock-1.11.9', 'PSX_TAS_RESUME_SPU_KEYOFF_FROM': 'unversioned'})
        for args in [
            ('default', 'unversioned', Path('native.pst'), True, source),
            ('octoshock-1.11.9', 'unversioned', None, True, source),
            ('octoshock-1.11.9', 'unversioned', Path('native.pst'), False, source),
            ('octoshock-1.11.9', 'foreign', Path('native.pst'), True, source),
            ('octoshock-1.11.9', None, None, False, 'default'),
            ('foreign', None, None, False, source),
        ]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                runner.spu_keyoff_resume_environment(*args)


if __name__ == '__main__':
    unittest.main()
