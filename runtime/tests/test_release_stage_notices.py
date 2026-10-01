#!/usr/bin/env python3
"""Pinning test for the licence files tools/release_stage.py writes beside a staged toolchain.

What this pins (PS1B-345). stage_toolchain() unpacks whole programs beside the
game: TinyCC, which is LGPL-2.1, and a Python interpreter. The pinned tcc archive
says "See COPYING file" and contains none, so every staged toolchain carried tcc
with a statement of its licence and no licence text. stage_toolchain() is the one
function every path uses -- a build host, a release packager, and the player's
machine during setup (a setup package does not contain tcc; setup downloads it)
-- so the licence files are written there:

  * licenses/toolchain/tcc-COPYING.txt   the LGPL-2.1 text, carried in tools/licenses/
  * licenses/toolchain/tcc-NOTICE.txt    which tcc release, and where its source is
  * overlay_toolchain/tcc/examples       not kept (five .c files nothing reads)

and a bundled program without its licence file is a WARNING that names the file,
never a failed staging: the same step writes the file, so a miss means a damaged
tree, and staging must not fall back to "overlays interpreted" over it.

Workbench Studio writes the same two files for a private product. The two SHA-256
values below are the same constants Studio's own test pins, so both paths ship
identical bytes.

Hermetic: synthetic archives, no download, no recompiler.
"""

import hashlib
import io
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.abspath(os.path.join(_HERE, '..', '..', 'tools'))
sys.path.insert(0, _TOOLS)

import release_stage as rs  # noqa: E402

# The same two values are pinned in Workbench Studio (corpus/tests/test_workbench_output.py).
LGPL_21_SHA256 = '20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95'
TCC_NOTICE_SHA256 = '7eb9c42aeebb28b5fec05fc065a4c22808aedfccd344ba3e75674d06f21e1835'


def touch(path, content='x'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(content)
    return path


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


class CarriedTextTest(unittest.TestCase):
    def test_the_carried_text_is_the_lgpl_2_1_and_matches_its_recorded_hash(self):
        with open(rs.LGPL_21_TEXT, 'rb') as f:
            data = f.read()
        self.assertIn(b'GNU LESSER GENERAL PUBLIC LICENSE', data)
        self.assertIn(b'Version 2.1, February 1999', data)
        self.assertNotIn(b'\r', data, 'a line-ending conversion would change the hash; see .gitattributes')
        self.assertEqual(hashlib.sha256(data).hexdigest(), LGPL_21_SHA256)
        self.assertEqual(rs.LGPL_21_SHA256, LGPL_21_SHA256)

    def test_the_notice_names_the_pinned_release_and_is_the_bytes_studio_writes(self):
        pins = rs.TOOLCHAIN_PINS['win']
        notice = rs.tcc_notice(pins)
        for part in ('TinyCC (tcc) 0.9.27', 'tcc-0.9.27-win64-bin.zip', pins['tcc_sha256'],
                     'https://download.savannah.gnu.org/releases/tinycc/', 'tcc-0.9.27.tar.bz2',
                     'Lesser General Public License, version 2.1', 'tcc-COPYING.txt',
                     # True on a build host and on the player's machine alike.
                     'unpacked when this product was built, or by its setup on this machine',
                     'which downloads it from that address'):
            self.assertIn(part, notice)
        self.assertEqual(hashlib.sha256(notice.encode('utf-8')).hexdigest(), TCC_NOTICE_SHA256)


class ToolchainNoticeTest(unittest.TestCase):
    """stage_toolchain() with synthetic archives, as test_release_stage_cache.py does it."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_relnotice_')
        self.stage = os.path.join(self.tmp, 'stage')
        self.tools = os.path.join(self.tmp, 'tools')
        self.framework = os.path.join(self.tmp, 'framework')
        self.include = os.path.join(self.framework, 'runtime', 'include')
        self.bios = os.path.join(self.framework, 'bios')
        self.recomp = os.path.join(self.tmp, 'recompiler')
        self.cache = os.path.join(self.tmp, 'dl')
        for d in (self.tools, self.include, self.bios, self.recomp):
            os.makedirs(d, exist_ok=True)
        touch(os.path.join(self.tools, 'compile_overlays.py'))
        touch(os.path.join(self.include, 'overlay_api.h'))
        touch(os.path.join(self.include, 'overlay_dispatch_preamble.c.inc'))
        touch(os.path.join(self.bios, 'SCPH1001.toml'))
        touch(os.path.join(self.recomp, 'psxrecomp-game.exe'))
        touch(os.path.join(self.recomp, 'psxrecomp-game'))

        self.py_zip = os.path.join(self.tmp, 'python.zip')
        with zipfile.ZipFile(self.py_zip, 'w') as z:
            z.writestr('python.exe', 'fake')
            z.writestr('LICENSE.txt', 'PSF licence')
        self.py_bare_zip = os.path.join(self.tmp, 'python-bare.zip')
        with zipfile.ZipFile(self.py_bare_zip, 'w') as z:
            z.writestr('python.exe', 'fake')
        self.tcc_zip = os.path.join(self.tmp, 'tcc.zip')
        with zipfile.ZipFile(self.tcc_zip, 'w') as z:
            z.writestr('tcc/tcc.exe', 'fake')
            z.writestr('tcc/include/stdio.h', '/* tcc */')
            z.writestr('tcc/doc/tcc-win32.txt', 'See COPYING file')
            for name in ('dll.c', 'fib.c', 'hello_dll.c', 'hello_win.c', 'libtcc_test.c'):
                z.writestr('tcc/examples/' + name, 'int main(void) { return 0; }')

        self.orig_pins = dict(rs.TOOLCHAIN_PINS)
        self.orig_fetch = rs.get_pinned_archive
        self.orig_text = rs.LGPL_21_TEXT
        real = self.orig_pins['win']
        rs.TOOLCHAIN_PINS['win'] = dict(real, python_url=self.py_zip, tcc_url=self.tcc_zip)
        # A platform with no pinned tcc, as Linux and macOS are, staged from the same zip shape.
        rs.TOOLCHAIN_PINS['linux'] = dict(self.orig_pins['linux'], python_url=self.py_zip)
        rs.get_pinned_archive = lambda url, _sha, _dest, log=print: url
        self.lines = []

    def tearDown(self):
        rs.TOOLCHAIN_PINS.clear()
        rs.TOOLCHAIN_PINS.update(self.orig_pins)
        rs.get_pinned_archive = self.orig_fetch
        rs.LGPL_21_TEXT = self.orig_text
        shutil.rmtree(self.tmp, ignore_errors=True)

    def stage_it(self, platform_tag='win'):
        self.lines = []
        # Linux looks for python/bin/python3; the fake zip has python.exe at the root.
        if platform_tag == 'linux':
            touch(os.path.join(self.stage, 'overlay_toolchain', 'python', 'bin', 'python3'))
            rs._extract_zip = lambda archive, dest: self._extract_keep(archive, dest)
        try:
            return rs.stage_toolchain(self.stage, self.recomp, self.tools, self.include, self.cache,
                                      platform_tag=platform_tag, log=self.lines.append)
        finally:
            rs._extract_zip = ToolchainNoticeTest._real_extract

    _real_extract = staticmethod(rs._extract_zip)

    def _extract_keep(self, archive, dest):
        ToolchainNoticeTest._real_extract(archive, dest)
        touch(os.path.join(dest, 'bin', 'python3'))

    def warnings(self):
        return [line for line in self.lines if str(line).startswith('WARNING')]

    def test_a_staged_tcc_gets_its_licence_text_and_notice_and_loses_its_examples(self):
        self.stage_it()
        licence = os.path.join(self.stage, 'licenses', 'toolchain', 'tcc-COPYING.txt')
        notice = os.path.join(self.stage, 'licenses', 'toolchain', 'tcc-NOTICE.txt')
        self.assertEqual(sha256(licence), LGPL_21_SHA256)
        with open(notice, 'rb') as f:
            data = f.read()
        self.assertNotIn(b'\r', data)
        self.assertIn(b'TinyCC (tcc) 0.9.27', data)
        tcc = os.path.join(self.stage, 'overlay_toolchain', 'tcc')
        self.assertFalse(os.path.exists(os.path.join(tcc, 'examples')))
        for kept in ('tcc.exe', os.path.join('include', 'stdio.h'), os.path.join('doc', 'tcc-win32.txt')):
            self.assertTrue(os.path.isfile(os.path.join(tcc, kept)), kept)
        # No source file is left in the staged toolchain except its headers.
        sources = [name for _root, _dirs, files in os.walk(os.path.join(self.stage, 'overlay_toolchain'))
                   for name in files if name.endswith(('.c', '.cpp'))]
        self.assertEqual(sources, [])
        self.assertEqual(rs.bundled_notice_gaps(self.stage), [])
        self.assertEqual(self.warnings(), [])

    def test_a_platform_with_no_pinned_tcc_gets_no_tcc_files_and_no_warning(self):
        self.stage_it('linux')
        self.assertFalse(os.path.exists(os.path.join(self.stage, 'licenses')))
        self.assertFalse(os.path.exists(os.path.join(self.stage, 'overlay_toolchain', 'tcc')))
        self.assertEqual(rs.bundled_notice_gaps(self.stage), [])
        self.assertEqual(self.warnings(), [])

    def test_a_damaged_tree_is_said_in_plain_words_and_the_toolchain_is_still_staged(self):
        # The carried text is missing: on the player's machine that means a damaged package.
        # Staging must finish, so the product still gets its compiler, and must say which file.
        rs.LGPL_21_TEXT = os.path.join(self.tmp, 'gone', 'LGPL-2.1.txt')
        toolchain = self.stage_it()
        self.assertTrue(os.path.isfile(os.path.join(toolchain, 'tcc', 'tcc.exe')))
        self.assertFalse(os.path.exists(os.path.join(self.stage, 'licenses', 'toolchain', 'tcc-COPYING.txt')))
        warnings = self.warnings()
        self.assertEqual(len(warnings), 2, warnings)
        self.assertIn('this package is damaged', warnings[0])
        self.assertIn(rs.LGPL_21_TEXT, warnings[0])
        self.assertIn('TinyCC (overlay_toolchain/tcc) needs licenses/toolchain/tcc-COPYING.txt', warnings[1])
        # An edited text is refused the same way: the bytes are checked before every copy.
        edited = touch(os.path.join(self.tmp, 'edited', 'LGPL-2.1.txt'), 'not the licence\n')
        rs.LGPL_21_TEXT = edited
        shutil.rmtree(self.stage)
        self.stage_it()
        self.assertFalse(os.path.exists(os.path.join(self.stage, 'licenses', 'toolchain', 'tcc-COPYING.txt')))
        self.assertIn('this package is damaged', self.warnings()[0])

    def test_a_python_without_its_licence_file_is_a_warning_that_names_it(self):
        rs.TOOLCHAIN_PINS['win']['python_url'] = self.py_bare_zip
        self.stage_it()
        self.assertEqual(rs.bundled_notice_gaps(self.stage),
                         ['the bundled Python (overlay_toolchain/python) has no LICENSE file of its own'])
        self.assertEqual(len(self.warnings()), 1)
        # A relocatable CPython keeps the file below lib/: found by name, not by a fixed path.
        touch(os.path.join(self.stage, 'overlay_toolchain', 'python', 'lib', 'python3.13', 'LICENSE.txt'))
        self.assertEqual(rs.bundled_notice_gaps(self.stage), [])

    def test_a_licence_file_with_other_text_is_reported(self):
        self.stage_it()
        touch(os.path.join(self.stage, 'licenses', 'toolchain', 'tcc-COPYING.txt'), 'LGPL, see the web\n')
        self.assertEqual(rs.bundled_notice_gaps(self.stage),
                         ['TinyCC: licenses/toolchain/tcc-COPYING.txt is not the LGPL-2.1 text'])


class CheckCommandTest(unittest.TestCase):
    """`check-bundled-notices`: the same check for a packager's stage. Warns and exits 0;
    --strict exits 1. A stage with no bundled program (a setup package) is a pass."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_relcheck_')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_check(self, *extra):
        out = io.StringIO()
        with redirect_stdout(out):
            code = rs.main(['check-bundled-notices', '--stage', self.tmp, *extra])
        return code, out.getvalue()

    def test_a_stage_with_no_bundled_program_passes(self):
        touch(os.path.join(self.tmp, 'Game.exe'))
        touch(os.path.join(self.tmp, 'psxrecomp', 'tools', 'release_stage.py'))
        self.assertEqual(self.run_check()[0], 0)
        self.assertEqual(self.run_check('--strict')[0], 0)

    def test_a_missing_licence_file_warns_and_only_strict_fails(self):
        touch(os.path.join(self.tmp, 'overlay_toolchain', 'tcc', 'tcc.exe'))
        code, text = self.run_check()
        self.assertEqual(code, 0)
        self.assertIn('WARNING: bundled program without its licence file: TinyCC (overlay_toolchain/tcc) needs '
                      'licenses/toolchain/tcc-COPYING.txt (LGPL-2.1)', text)
        self.assertEqual(self.run_check('--strict')[0], 1)
        # With the file in place both pass.
        os.makedirs(os.path.join(self.tmp, 'licenses', 'toolchain'))
        shutil.copyfile(rs.LGPL_21_TEXT, os.path.join(self.tmp, 'licenses', 'toolchain', 'tcc-COPYING.txt'))
        self.assertEqual(self.run_check('--strict')[0], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
