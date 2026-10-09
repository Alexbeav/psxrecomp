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


if __name__ == '__main__':
    unittest.main()
