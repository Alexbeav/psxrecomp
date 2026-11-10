#!/usr/bin/env python3
"""extract_generic's normal-mode discovery must not depend on the process cwd,
and must fail closed when the recompiler does not complete.

Before this test, full_discovery_seeds ran psxrecomp-game with no
--project-root and ignored its exit code. From a cwd without bios/ the
recompiler exits "no BIOS profile found", the pipeline fell back to reduced
seeds, and aot_overlay_pipeline release shipped fewer native pairs with no
error (Tomba 2: 54 pairs instead of 81).

Build/run: ctest -R extract_discovery_project_root
"""
import argparse
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'aot_overlay_spike'))
sys.path.insert(0, os.path.join(HERE, '..'))
import extract_generic as eg  # noqa: E402

RECOMPILER = None
BASE = 0x80010000
# f: jal g; nop; jr ra; nop   g: jr ra; nop
BODY = b''.join(w.to_bytes(4, 'little') for w in (
    0x0C000000 | ((BASE + 0x10) >> 2) & 0x03FFFFFF, 0, 0x03E00008, 0,
    0x03E00008, 0))


def fake_recompiler(directory, exit_code, message):
    """An executable that prints `message` and exits `exit_code`."""
    if os.name == 'nt':
        path = os.path.join(directory, 'fake_recompiler.cmd')
        with open(path, 'w') as f:
            f.write(f'@echo off\r\necho {message} 1>&2\r\nexit /b {exit_code}\r\n')
    else:
        path = os.path.join(directory, 'fake_recompiler.sh')
        with open(path, 'w') as f:
            f.write(f'#!/bin/sh\necho "{message}" >&2\nexit {exit_code}\n')
        os.chmod(path, 0o755)
    return path


class DiscoveryProjectRoot(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cwd = os.getcwd()
        # A cwd with no bios/ and no framework below it: the case that
        # silently degraded.
        self.empty_cwd = os.path.join(self.tmp.name, 'elsewhere')
        os.makedirs(self.empty_cwd)
        os.chdir(self.empty_cwd)

    def tearDown(self):
        os.chdir(self.cwd)
        self.tmp.cleanup()

    def work(self, name):
        path = os.path.join(self.tmp.name, name)
        os.makedirs(path, exist_ok=True)
        return path

    def test_discovers_from_a_cwd_without_bios(self):
        if not RECOMPILER:
            self.skipTest('--recompiler not given')
        data = eg.make_psx_exe(BODY, BASE, BASE)
        seeds, _ = eg.full_discovery_seeds(data, RECOMPILER, self.work('real'))
        self.assertIsNotNone(seeds, 'normal mode found no entry from a cwd without bios/')
        self.assertIn(BASE, seeds)
        self.assertIn(BASE + 0x10, seeds)

    def test_failed_run_raises_with_its_message(self):
        fake = fake_recompiler(self.tmp.name, 1, 'FATAL: no BIOS profile found')
        with self.assertRaises(eg.DiscoveryError) as cm:
            eg.full_discovery_seeds(eg.make_psx_exe(BODY, BASE, BASE), fake,
                                    self.work('fail'))
        self.assertIn('exit 1', str(cm.exception))
        self.assertIn('no BIOS profile found', str(cm.exception))

    def test_unrunnable_recompiler_raises(self):
        missing = os.path.join(self.tmp.name, 'no_such_recompiler.exe')
        with self.assertRaises(eg.DiscoveryError):
            eg.full_discovery_seeds(eg.make_psx_exe(BODY, BASE, BASE), missing,
                                    self.work('missing'))

    def test_root_without_profile_is_refused(self):
        with self.assertRaises(eg.DiscoveryError):
            eg.discovery_project_root(self.empty_cwd)

    def test_default_root_is_this_framework(self):
        root = eg.discovery_project_root()
        self.assertTrue(os.path.isfile(os.path.join(root, 'bios', 'SCPH1001.toml')))
        self.assertEqual(os.path.normcase(root),
                         os.path.normcase(os.path.abspath(os.path.join(HERE, '..', '..'))))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--recompiler')
    args, rest = ap.parse_known_args()
    RECOMPILER = os.path.abspath(args.recompiler) if args.recompiler else None
    unittest.main(argv=[sys.argv[0]] + rest)
