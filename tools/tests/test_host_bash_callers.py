"""Authored Windows shell-selection controls. No BIOS, compiler or game runs."""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import host_bash

BASELINE_CLI = None
BASELINE_CMAKE = None


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class BashCallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='psx-bash-callers-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.windows = self.root / 'Windows'
        self.wsl = self.windows / 'System32' / 'bash.exe'
        self.git = self.root / 'Git with spaces' / 'bin' / 'bash.exe'
        for path in (self.wsl, self.git):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'')
        self.env = {'SystemRoot': str(self.windows),
                    'PATH': os.pathsep.join((str(self.wsl.parent), str(self.git.parent)))}
        self.real_find = host_bash.find_bash

    def windows_find(self, purpose, **kwargs):
        kwargs['windows'] = True
        return self.real_find(purpose, **kwargs)

    @contextlib.contextmanager
    def policy(self):
        with patch.dict(os.environ, self.env, clear=True), \
                patch.object(host_bash, 'find_bash', side_effect=self.windows_find), \
                patch.object(host_bash, 'is_msys_bash', return_value=True):
            yield

    def test_cli_fingerprint_never_executes_the_first_path_launcher(self):
        cli = load('_host_bash_cli', BASELINE_CLI or ROOT / 'psxrecomp_cli.py')
        fw = self.root / 'framework'
        (fw / 'tools').mkdir(parents=True)
        (fw / 'tools' / 'bios_emitter_fingerprint.sh').write_text('authored stand-in\n', encoding='utf-8')
        (fw / 'profile.toml').write_text('[recompiler]\nout_stem="Authored"\n', encoding='utf-8')
        calls = []
        def fingerprint(argv, **kwargs):
            calls.append(argv)
            self.assertEqual(Path(argv[0]), self.git, 'WSL launcher reached subprocess.run')
            return subprocess.CompletedProcess(argv, 0, 'authored-fingerprint\n', '')
        with self.policy(), patch.object(cli.subprocess, 'run', side_effect=fingerprint):
            if hasattr(cli, 'find_bash'):
                with patch.object(cli, 'find_bash', side_effect=self.windows_find):
                    cli.write_bios_emitter_stamp(fw, 'profile.toml', progress=SimpleNamespace(log=lambda line: None))
            else:
                cli.write_bios_emitter_stamp(fw, 'profile.toml', progress=SimpleNamespace(log=lambda line: None))
        self.assertEqual(len(calls), 1)
        self.assertEqual((fw / 'generated' / 'Authored.emitter.sha').read_text(), 'authored-fingerprint\n')

    def test_optional_stamp_refuses_an_invalid_explicit_override(self):
        if BASELINE_CLI:
            self.skipTest('baseline measurement selects the unsafe CLI seam only')
        cli = load('_host_bash_cli_optional', ROOT / 'psxrecomp_cli.py')
        fw = self.root / 'optional'
        (fw / 'tools').mkdir(parents=True)
        (fw / 'tools' / 'bios_emitter_fingerprint.sh').write_text('stand-in', encoding='utf-8')
        messages = []
        with self.policy(), patch.dict(os.environ, {'PSX_GIT_BASH': str(self.wsl)}), \
                patch.object(cli, 'find_bash', side_effect=self.windows_find), \
                patch.object(cli.subprocess, 'run') as run:
            cli.write_bios_emitter_stamp(fw, 'missing-profile.toml', progress=SimpleNamespace(log=messages.append))
            run.assert_not_called()
        self.assertTrue(any('WSL launcher' in message for message in messages))

    def test_every_python_adapter_uses_the_same_policy(self):
        modules = [('package BIOS hint', 'tools/tests/test_package_setup_bios_hint.py'),
                   ('package program set', 'tools/tests/test_package_setup_program_set.py'),
                   ('BIOS fingerprint', 'recompiler/tests/test_bios_emitter_fingerprint.py'),
                   ('package gate', 'runtime/tests/test_setup_private_path_gate.py'),
                   ('staged-tree gate', 'runtime/tests/test_setup_staged_tree_private_paths.py')]
        for index, (label, relative) in enumerate(modules):
            with self.subTest(caller=label):
                module = load(f'_host_bash_adapter_{index}', ROOT / relative)
                with self.policy():
                    self.assertEqual(Path(module.find_bash()), self.git)
        sys.path.insert(0, str(ROOT / 'tools' / 'tasreplays'))
        media = load('_host_bash_media', ROOT / 'tools/tasreplays/media_container.py')
        with self.policy():
            self.assertEqual(media.find_git_bash(), self.git.resolve())
        replay = load('_host_bash_replay', ROOT / 'runtime/tests/replay_determinism/run.py')
        with self.policy(), patch.object(replay, 'find_bash', side_effect=self.windows_find):
            self.assertEqual(replay.host_bash(), self.git)

    def test_public_studio_regeneration_uses_the_same_policy_without_building(self):
        sys.path.insert(0, str(ROOT / 'tools' / 'new_project_layout'))
        from project_studio import buildops
        fw = self.root / 'studio-framework'
        (fw / 'tools').mkdir(parents=True)
        (fw / 'bios').mkdir()
        (fw / 'tools' / 'regen_bios.sh').write_text('stand-in', encoding='utf-8')
        (fw / 'bios' / 'OpenBIOS.toml').write_text('authored profile', encoding='utf-8')
        with self.policy(), patch.object(buildops, 'find_bash', side_effect=self.windows_find), \
                patch.object(buildops, '_run_stream') as run:
            result = buildops._regen_bios_profile(fw, 'bios/OpenBIOS.toml', dry_run=True)
            self.assertTrue(result.ok)
            self.assertIn(str(self.git), result.message)
            run.assert_not_called()

    def test_public_studio_backend_setup_checks_both_shell_sites(self):
        sys.path.insert(0, str(ROOT / 'tools' / 'new_project_layout'))
        from project_studio import buildops
        fw = self.root / 'studio-backends'
        (fw / 'tools').mkdir(parents=True)
        (fw / 'bios').mkdir()
        (fw / 'tools' / 'regen_bios.sh').write_text('stand-in', encoding='utf-8')
        (fw / 'bios' / 'OpenBIOS.toml').write_text('authored profile', encoding='utf-8')
        with self.policy(), patch.object(buildops, 'find_bash', side_effect=self.windows_find) as find, \
                patch.object(buildops, 'resolve_framework_root', return_value=fw), \
                patch.object(buildops, '_any_usable_recompiler_build', return_value=True), \
                patch.object(buildops, 'ensure_bios_emitter') as build:
            result = buildops.ensure_bios_backends(fw, include_scph1001=False, dry_run=True)
            self.assertTrue(result.ok)
            self.assertEqual(find.call_count, 2)
            build.assert_not_called()

    def test_public_studio_cross_command_uses_the_shared_shell(self):
        sys.path.insert(0, str(ROOT / 'tools' / 'new_project_layout'))
        from project_studio import cli
        toolkit = self.root / 'studio-toolkit'
        (toolkit / 'scripts').mkdir(parents=True)
        (toolkit / 'scripts' / 'build_windows_mingw.sh').write_text('stand-in', encoding='utf-8')
        args = SimpleNamespace(build_dir='build-mingw')
        with self.policy(), patch.object(cli, '_TOOLKIT', toolkit), \
                patch.object(cli, 'find_bash', side_effect=self.windows_find), \
                patch.object(subprocess, 'run', return_value=SimpleNamespace(returncode=0)) as run, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cli.cmd_build_mingw(args), 0)
            self.assertEqual(Path(run.call_args.args[0][0]), self.git)

    def test_native_lookup_and_explicit_overrides(self):
        with patch.object(host_bash.shutil, 'which', return_value='/usr/bin/bash'):
            self.assertEqual(self.real_find('native control', environ={'PATH': '/usr/bin'}, windows=False), '/usr/bin/bash')
        for explicit in (self.wsl, self.root / 'missing.exe'):
            with self.subTest(override=explicit), patch.object(host_bash.subprocess, 'run') as run:
                with self.assertRaises(AssertionError):
                    self.real_find('override control', environ={**self.env, 'PSX_GIT_BASH': str(explicit)}, windows=True)
                run.assert_not_called()
        self.assertEqual(self.real_find('valid override', environ={**self.env, 'PSX_GIT_BASH': str(self.git)}, windows=True), str(self.git))

    def cmake_find(self, *, cached=None, override=None, path=None, windows_root=None):
        cmake = os.environ.get('PSX_TEST_CMAKE') or shutil.which('cmake')
        if not cmake:
            self.skipTest('standalone check needs CMake; CTest passes its actual CMAKE_COMMAND')
        script = self.root / 'find-bash.cmake'
        script.write_text('set(CMAKE_HOST_WIN32 TRUE)\n'
                          f'set(_psxrt_bash "{cached.as_posix() if cached else ""}")\n'
                          f'include("{(BASELINE_CMAKE or ROOT / "runtime/host_bash.cmake").as_posix()}")\n'
                          'psxrecomp_find_bash(picked launcher)\n'
                          'message("PICK=${picked}\\nLAUNCHER=${launcher}")\n', encoding='utf-8')
        env = {**os.environ, **self.env}
        for name in ('ProgramFiles', 'ProgramW6432', 'ProgramFiles(x86)', 'LOCALAPPDATA'):
            env.pop(name, None)
        env.pop('PSX_GIT_BASH', None)
        if override is not None:
            env['PSX_GIT_BASH'] = str(override)
        if path is not None:
            env['PATH'] = os.pathsep.join(str(directory) for directory in path)
        if windows_root is not None:
            env['SystemRoot'] = str(windows_root)
        # CMake converts PATH using its real host separator. This does not execute Bash.
        return subprocess.run([cmake, '-P', str(script)], env=env,
                              capture_output=True, text=True, encoding='utf-8', errors='replace',
                              timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))

    def directory_alias(self, link, target):
        # All targets are empty authored fixtures inside this temporary directory.
        self.assertTrue(link.parent.resolve().is_relative_to(self.root.resolve()))
        self.assertTrue(target.resolve().is_relative_to(self.root.resolve()))
        if os.name == 'nt':
            powershell = shutil.which('pwsh') or shutil.which('powershell')
            if not powershell:
                self.skipTest('authored directory junction needs PowerShell')
            script = self.root / 'make-junction.ps1'
            script.write_text('param([string]$Link, [string]$Target)\n'
                              'New-Item -ItemType Junction -Path $Link -Value $Target '
                              '-ErrorAction Stop | Out-Null\n', encoding='utf-8')
            proc = subprocess.run([powershell, '-NoLogo', '-NoProfile', '-NonInteractive',
                                   '-File', str(script), '-Link', str(link), '-Target', str(target)],
                                  capture_output=True, text=True, encoding='utf-8', errors='replace',
                                  timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
            if proc.returncode:
                self.skipTest('authored junction unavailable: ' + proc.stdout + proc.stderr)
            self.addCleanup(link.rmdir)
        else:
            try:
                link.symlink_to(target, target_is_directory=True)
            except OSError as exc:
                self.skipTest('authored directory symlink unavailable: ' + str(exc))
            self.addCleanup(link.unlink)
        self.assertEqual(link.resolve(), target.resolve())
        return link

    def test_cmake_finder_skips_launchers_and_honours_override(self):
        for explicit, good in ((None, True), (self.git, True), (self.wsl, False), (self.root / 'missing.exe', False)):
            with self.subTest(override=explicit):
                proc = self.cmake_find(cached=self.wsl, override=explicit)
                if good:
                    self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                    self.assertIn('PICK=' + self.git.as_posix(), (proc.stdout + proc.stderr).replace('\\', '/'))
                else:
                    self.assertNotEqual(proc.returncode, 0)
                    self.assertIn('PSX_GIT_BASH', proc.stdout + proc.stderr)

    def test_cmake_finder_refuses_aliases_in_override_cache_and_path(self):
        alias = self.directory_alias(self.root / 'launcher alias', self.wsl.parent) / 'bash.exe'
        for route in ('override', 'cache', 'PATH'):
            with self.subTest(route=route):
                proc = self.cmake_find(override=alias if route == 'override' else None,
                                       cached=alias if route == 'cache' else None,
                                       path=[alias.parent, self.git.parent])
                output = (proc.stdout + proc.stderr).replace('\\', '/')
                if route == 'override':
                    self.assertNotEqual(proc.returncode, 0, output)
                    self.assertIn('PSX_GIT_BASH', output)
                else:
                    self.assertEqual(proc.returncode, 0, output)
                    self.assertIn('PICK=' + self.git.as_posix(), output)

    def test_cmake_finder_resolves_root_and_retains_windowsapps_spelling(self):
        root_alias = self.directory_alias(self.root / 'root alias', self.windows)
        apps_parent = self.root / 'Microsoft'
        apps_parent.mkdir()
        apps = self.directory_alias(apps_parent / 'WindowsApps', self.git.parent) / 'bash.exe'
        for explicit, windows_root in ((self.wsl, root_alias), (apps, self.windows)):
            with self.subTest(override=explicit):
                proc = self.cmake_find(override=explicit, windows_root=windows_root)
                self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn('PSX_GIT_BASH', proc.stdout + proc.stderr)

    def test_cmake_finder_accepts_spaced_git_aliases(self):
        alias = self.directory_alias(self.root / 'Git alias with spaces', self.git.parent) / 'bash.exe'
        for route in ('override', 'cache', 'PATH'):
            with self.subTest(route=route):
                proc = self.cmake_find(override=alias if route == 'override' else None,
                                       cached=alias if route == 'cache' else None, path=[alias.parent])
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn('PICK=' + alias.as_posix(), (proc.stdout + proc.stderr).replace('\\', '/'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument('--baseline-cli', type=Path)
    baseline.add_argument('--baseline-cmake', type=Path)
    args, remaining = parser.parse_known_args()
    BASELINE_CLI = args.baseline_cli
    BASELINE_CMAKE = args.baseline_cmake
    suite = (unittest.TestSuite([BashCallerTests('test_cli_fingerprint_never_executes_the_first_path_launcher')])
             if BASELINE_CLI else
             unittest.TestSuite([BashCallerTests('test_cmake_finder_refuses_aliases_in_override_cache_and_path')])
             if BASELINE_CMAKE else unittest.defaultTestLoader.loadTestsFromTestCase(BashCallerTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
