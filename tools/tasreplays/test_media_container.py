"""Container admission and Git bash discovery, without retail assets.

A .chd only ever reaches the existing pinned checks by being split back to the
original layout, so these tests drive the real verifiers (tekken3.verify_cue,
pepsiman.media) over a mocked chdman and confirm both the accept and the reject.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import biohazard
import media_container
import nymashock_admission as source
import pepsiman
import tekken3

REDUMP_SINGLE = 'FILE "{name}.bin" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n'


def fake_chdman(payloads):
    """Stand in for `chdman extractcd -sb`: one .bin per track, plus its cue."""
    def run(argv, **kwargs):
        argv = [str(value) for value in argv]
        assert argv[1] == 'extractcd' and '-sb' in argv, argv
        cue = Path(argv[argv.index('-o') + 1])
        text = ''
        for index, payload in enumerate(payloads, 1):
            # chdman always suffixes the track number, even for a lone track.
            name = f'{cue.stem} (Track {index}).bin'
            (cue.parent / name).write_bytes(payload)
            text += f'FILE "{name}" BINARY\r\n  TRACK {index:02d} '
            text += ('MODE2/2352\r\n    INDEX 01 00:00:00\r\n' if index == 1 else
                     'AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n')
        cue.write_bytes(text.encode())
        return subprocess.CompletedProcess(argv, 0, '', '')
    return run


def identities(payloads):
    return [(len(payload), hashlib.sha256(payload).hexdigest()) for payload in payloads]


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.cache = self.root / 'cache'
        self.addCleanup(self.temporary.cleanup)

    def chd(self, name='disc.chd', body=b'container'):
        path = self.root / name
        path.write_bytes(body)
        return path

    def split(self, chd, payloads, **kwargs):
        with patch.object(media_container, 'find_chdman', return_value=Path('chdman.exe')), \
                patch('media_container.subprocess.run', side_effect=fake_chdman(payloads)) as runner:
            cue = media_container.resolve_disc(chd, cache=self.cache, **kwargs)
        return cue, runner

    def test_cue_input_keeps_its_current_path(self):
        cue = self.root / 'game.cue'
        cue.write_text('FILE "1.bin" BINARY\n')
        # No container work at all: chdman is never looked for, let alone run.
        with patch.object(media_container, 'find_chdman', side_effect=AssertionError):
            for supplied in (cue, self.root / 'GAME.CUE', self.root / 'missing.cue'):
                with self.subTest(supplied=supplied):
                    self.assertEqual(media_container.resolve_disc(supplied, cache=self.cache), supplied)
        self.assertFalse(self.cache.exists())

    def test_three_track_container_is_admitted_by_the_existing_verifier(self):
        payloads = [b'data-track', b'audio-two', b'audio-three-longer']
        chd = self.chd('Tekken 3 (USA).chd')
        cue, runner = self.split(chd, payloads)
        self.assertEqual(runner.call_count, 1)
        # Deterministic, keyed by the container's own bytes.
        self.assertEqual(cue.parent.parent, self.cache.resolve())
        self.assertEqual(cue.parent.name, hashlib.sha256(chd.read_bytes()).hexdigest())
        self.assertEqual(cue.name, 'Tekken 3 (USA).cue')
        with patch.object(tekken3, 'TRACKS', identities(payloads)):
            self.assertEqual([path.name for path in tekken3.verify_cue(cue)],
                             [f'Tekken 3 (USA) (Track {n}).bin' for n in (1, 2, 3)])

    def test_eight_track_container_is_admitted_by_the_existing_verifier(self):
        payloads = [b'data'] + [b'cdda-%02d' % n for n in range(2, 9)]
        cue, _ = self.split(self.chd('Pepsiman (Japan).chd'), payloads)
        bios = self.root / 'bios.bin'
        bios.write_bytes(b'firmware')
        with patch.object(pepsiman, 'TRACKS', identities(payloads)), \
                patch.object(pepsiman, 'BIOS_SHA', hashlib.sha256(b'firmware').hexdigest()):
            tracks = pepsiman.media(cue, bios)
            self.assertEqual([path.name for path in tracks],
                             [f'Pepsiman (Japan) (Track {n}).bin' for n in range(1, 9)])
            (cue.parent / 'Pepsiman (Japan) (Track 7).bin').write_bytes(b'cdda-xx')
            with self.assertRaisesRegex(ValueError, 'Wrong or missing'):
                pepsiman.media(cue, bios)

    def test_single_track_container_takes_the_original_dump_name(self):
        # Redump names a one-track dump '<stem>.bin'; harnesses that pin the cue
        # file's own bytes only match if the FILE line carries that exact name.
        cue, _ = self.split(self.chd('anything.chd'), [b'data-track'], stem=biohazard.DISC_STEM)
        self.assertEqual(cue.name, biohazard.DISC_STEM + '.cue')
        self.assertEqual(cue.read_bytes(), REDUMP_SINGLE.format(name=biohazard.DISC_STEM).encode())
        self.assertTrue((cue.parent / (biohazard.DISC_STEM + '.bin')).is_file())
        self.assertFalse(list(cue.parent.glob('*(Track 1)*')))

    def test_bio_hazard_round_trip_reproduces_the_pinned_cue_bytes(self):
        rebuilt = REDUMP_SINGLE.format(name=biohazard.DISC_STEM).encode()
        self.assertEqual(hashlib.sha256(rebuilt).hexdigest(),
                         source.FIXED[biohazard.DISC_STEM + '.cue'])

    def test_wrong_content_container_fails_exactly_like_a_wrong_cue(self):
        cue, _ = self.split(self.chd(), [b'data-track', b'audio-two', b'audio-thref'])
        same_size = identities([b'data-track', b'audio-two', b'audio-three'])
        with patch.object(tekken3, 'TRACKS', same_size):
            with self.assertRaisesRegex(ValueError, 'Wrong or missing'):
                tekken3.verify_cue(cue)
        other_size = identities([b'data-track', b'audio-two', b'audio-three-longer'])
        with patch.object(tekken3, 'TRACKS', other_size):
            with self.assertRaisesRegex(ValueError, 'wrong size'):
                tekken3.verify_cue(cue)

    def test_wrong_track_count_container_is_rejected(self):
        cue, _ = self.split(self.chd(), [b'data-track', b'audio-two'])
        with self.assertRaisesRegex(ValueError, 'three-track'):
            tekken3.verify_cue(cue)

    def test_extracted_set_is_reused_not_re_split(self):
        payloads = [b'data-track', b'audio-two', b'audio-three']
        chd = self.chd()
        first, _ = self.split(chd, payloads)
        second, runner = self.split(chd, payloads)
        self.assertEqual(first, second)
        self.assertEqual(runner.call_count, 0)
        # A different container never reuses another's extraction.
        other, _ = self.split(self.chd('other.chd', b'different'), payloads)
        self.assertNotEqual(other.parent, first.parent)

    def test_failed_extraction_leaves_no_reusable_cache_entry(self):
        chd = self.chd()
        def failing(argv, **kwargs):
            return subprocess.CompletedProcess(argv, 1, 'chdman: bad checksum', '')
        with patch.object(media_container, 'find_chdman', return_value=Path('chdman.exe')), \
                patch('media_container.subprocess.run', failing):
            with self.assertRaisesRegex(ValueError, 'could not extract'):
                media_container.resolve_disc(chd, cache=self.cache)
        key = hashlib.sha256(chd.read_bytes()).hexdigest()
        self.assertFalse((self.cache / key).exists())
        self.assertEqual(list(self.cache.glob('*')), [])

    def test_chdman_lookup_prefers_the_explicit_tool(self):
        explicit, environment, on_path = (self.root / name for name in
                                          ('explicit.exe', 'environment.exe', 'path.exe'))
        for candidate in (explicit, environment, on_path):
            candidate.write_bytes(b'')
        with patch.dict(os.environ, {'PSX_CHDMAN': str(environment)}), \
                patch.object(media_container.shutil, 'which', return_value=str(on_path)):
            self.assertEqual(media_container.find_chdman(explicit), explicit.resolve())
            self.assertEqual(media_container.find_chdman(), environment.resolve())
            environment.unlink()
            self.assertEqual(media_container.find_chdman(), on_path.resolve())
        with patch.dict(os.environ, {'PSX_CHDMAN': str(self.root / 'absent.exe')}), \
                patch.object(media_container.shutil, 'which', return_value=None), \
                patch.object(media_container, 'CHDMAN_FALLBACKS', ()):
            with self.assertRaisesRegex(ValueError, 'chdman is required'):
                media_container.find_chdman()


class BashDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.git = self.root / 'Git'
        for relative in ('cmd/git.exe', 'mingw64/bin/git.exe', 'bin/bash.exe', 'usr/bin/bash.exe'):
            (self.git / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.git / relative).write_bytes(b'')
        self.addCleanup(self.temporary.cleanup)
        media_container._bash = None
        self.addCleanup(setattr, media_container, '_bash', None)

    def candidates(self, git):
        exec_path = str(self.git / 'mingw64/libexec/git-core')
        with patch.object(media_container.shutil, 'which', return_value=str(git)), \
                patch.object(media_container.subprocess, 'check_output', return_value=exec_path + '\n'):
            return media_container.bash_candidates()

    def test_both_git_locations_reach_the_same_bash(self):
        # From PowerShell git is Git/cmd/git.exe; from Git Bash it is
        # Git/mingw64/bin/git.exe, where the old parents[1] guess is wrong.
        wanted = self.git / 'bin/bash.exe'
        resolved = {}
        for label, git in (('powershell', self.git / 'cmd/git.exe'),
                           ('git-bash', self.git / 'mingw64/bin/git.exe')):
            with self.subTest(launcher=label):
                self.assertIn(wanted, self.candidates(git))
                if label == 'git-bash':
                    # The old derivation, which(git).parents[1]/'bin/bash.exe',
                    # is exactly what broke here: that path does not exist.
                    self.assertFalse((git.parents[1] / 'bin/bash.exe').exists())
                media_container._bash = None
                with patch.object(media_container.shutil, 'which', return_value=str(git)), \
                        patch.object(media_container.subprocess, 'check_output',
                                     return_value=str(self.git / 'mingw64/libexec/git-core')), \
                        patch.object(media_container, 'is_msys_bash', lambda path: Path(path) == wanted):
                    resolved[label] = media_container.find_git_bash()
        self.assertEqual(resolved['powershell'], resolved['git-bash'])
        self.assertEqual(resolved['powershell'], wanted.resolve())

    def test_bash_is_still_found_when_git_exec_path_is_unavailable(self):
        wanted = self.git / 'usr/bin/bash.exe'
        with patch.object(media_container.shutil, 'which', return_value=str(self.git / 'cmd/git.exe')), \
                patch.object(media_container.subprocess, 'check_output', side_effect=OSError), \
                patch.object(media_container, 'is_msys_bash', lambda path: Path(path) == wanted):
            self.assertEqual(media_container.find_git_bash(), wanted.resolve())

    def test_wsl_launcher_is_rejected_without_being_run(self):
        windows = self.root / 'Windows'
        wsl = windows / 'System32/bash.exe'
        wsl.parent.mkdir(parents=True)
        wsl.write_bytes(b'')
        with patch.dict(os.environ, {'SystemRoot': str(windows)}), \
                patch.object(media_container.subprocess, 'run', side_effect=AssertionError):
            self.assertFalse(media_container.is_msys_bash(wsl))
        self.assertFalse(media_container.is_msys_bash(self.root / 'absent.exe'))

    def test_explicit_override_must_be_a_git_bash(self):
        with patch.dict(os.environ, {'PSX_GIT_BASH': str(self.git / 'bin/bash.exe')}), \
                patch.object(media_container, 'is_msys_bash', return_value=False):
            with self.assertRaisesRegex(ValueError, 'PSX_GIT_BASH is not'):
                media_container.find_git_bash()
        media_container._bash = None
        with patch.dict(os.environ, {'PSX_GIT_BASH': str(self.git / 'usr/bin/bash.exe')}), \
                patch.object(media_container, 'is_msys_bash', return_value=True):
            self.assertEqual(media_container.find_git_bash(), (self.git / 'usr/bin/bash.exe').resolve())

    @unittest.skipUnless(os.name == 'nt' and shutil.which('git'), 'needs an installed Git for Windows')
    def test_installed_git_bash_is_located_and_verified(self):
        bash = media_container.find_git_bash()
        self.assertTrue(bash.is_file())
        self.assertTrue(media_container.is_msys_bash(bash))


if __name__ == '__main__':
    unittest.main()
