#!/usr/bin/env python3
"""Unit tests for tools/package_title_release.py (no build, no disc, no game).

Pins the release contract every title now shares: --channel is required with
no default, owned CMake flags cannot be overridden by a title, artifact names
follow the channel, forbidden content fails the payload gates, diagnostics
defaults follow the channel, and a finished ZIP is re-verified by extraction.
"""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_title_release as ptr  # noqa: E402

DIAG = '''format_version = 5
id = "fixture.diagnostics"
version = "1.0.0"
name = "Playtest Diagnostics"
channel = "stable"

[[target]]
game_id = "SLUS-00000"

[[feature]]
id = "recording"
default_enabled = {on}

[[feature]]
id = "osd"
default_enabled = {on}

[[plugin]]
feature = "recording"
id = "fixture.diagnostics"

[[plugin]]
feature = "osd"
id = "fixture.diagnostics.osd"
'''

CONFIG = '''schema = 1

[product]
name = "FixtureRecomp"
display_name = "Fixture Recompiled"
exe_name = "Fixture"
version_file = "VERSION"

[build]
cmake_args = ["-DPSX_PGXP_VARIANT=OFF"]

[diagnostics]
game_id = "SLUS-00000"
package_id = "fixture.diagnostics"

[payload]
start_here = "START_HERE.txt"
files = ["README.md", {{ from = "notes/input.ini", to = "input.ini" }}]
{extra}
'''


class Fixture:
    def __init__(self, extra=''):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'VERSION').write_text('v1.2.3\n')
        (self.root / 'START_HERE.txt').write_text('Fixture @VERSION@\n')
        (self.root / 'README.md').write_text('readme\n')
        (self.root / 'game.toml').write_text('[game]\nid = "SLUS-00000"\n')
        (self.root / 'notes').mkdir()
        (self.root / 'notes/input.ini').write_text('[controller]\n')
        (self.root / 'release.toml').write_text(CONFIG.format(extra=extra))

    def config(self):
        return ptr.Config(self.root / 'release.toml', self.root)

    def close(self):
        self.tmp.cleanup()


def write_stage(stage, channel, version='v1.2.3', exe='Fixture.exe'):
    stage.mkdir(parents=True)
    (stage / exe).write_bytes(b'MZ fixture')
    (stage / 'psx_game_version.txt').write_text(version + '\n')
    (stage / 'START_HERE.txt').write_text('Fixture %s\n' % version)
    (stage / 'game.toml').write_text('[game]\nid = "SLUS-00000"\n')
    (stage / 'replay-diagnostics.py').write_text('# helper\n')
    (stage / 'bios').mkdir()
    (stage / 'bios/openbios.bin').write_bytes(b'\0' * ptr.OPENBIOS_SIZE)
    package = stage / 'mods/bundled/fixture.diagnostics/1.0.0'
    package.mkdir(parents=True)
    (package / 'manifest.toml').write_text(
        DIAG.format(on='true' if channel == 'playtest' else 'false'))
    files = {p.relative_to(stage).as_posix(): ptr.sha256(p)
             for p in sorted(stage.rglob('*')) if p.is_file()}
    manifest = dict(channel=channel, version=version, product='FixtureRecomp', executable=exe,
                    mod_packages=['fixture.diagnostics'],
                    playtest_diagnostics_default_enabled=channel == 'playtest', files=files)
    (stage / 'RELEASE_MANIFEST.json').write_text(json.dumps(manifest))


def zip_stage(stage, artifact):
    with zipfile.ZipFile(artifact, 'w') as archive:
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(stage).as_posix())
    Path(str(artifact) + '.sha256').write_text(
        '%s  %s\n' % (hashlib.sha256(artifact.read_bytes()).hexdigest(), artifact.name))


class ChannelIsRequired(unittest.TestCase):
    def run_main(self, argv):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as raised:
            ptr.main(argv)
        return raised.exception.code, err.getvalue()

    def test_missing_channel_fails_loudly(self):
        code, err = self.run_main(['--platform', 'windows'])
        self.assertNotEqual(code, 0)
        self.assertIn('PACKAGING FAILED', err)
        self.assertIn('--channel', err)

    def test_unknown_channel_fails(self):
        code, err = self.run_main(['--channel', 'release'])
        self.assertNotEqual(code, 0)
        self.assertIn('PACKAGING FAILED', err)

    def test_verify_also_requires_channel(self):
        code, _ = self.run_main(['verify', 'x.zip'])
        self.assertNotEqual(code, 0)

    def test_default_subcommand_is_package(self):
        args = ptr.parse_args(['--channel', 'playtest'])
        self.assertEqual((args.command, args.channel), ('package', 'playtest'))


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)

    def test_loads(self):
        config = self.fixture.config()
        self.assertEqual(config.version(), 'v1.2.3')
        self.assertEqual([rel for _, rel in config.files], ['README.md', 'input.ini'])
        self.assertEqual(config.aot_mode, 'none')
        self.assertTrue(config.diagnostics['replay_helper'])

    def test_owned_cmake_flags_are_refused(self):
        for flag in ('PSX_PUBLIC_RELEASE', 'PSX_RELEASE_CHANNEL', 'PSX_DEBUG_TOOLS',
                     'PSXRECOMP_BIOS_STEMS', 'CMAKE_BUILD_TYPE'):
            text = CONFIG.format(extra='').replace(
                '"-DPSX_PGXP_VARIANT=OFF"', '"-D%s=ON"' % flag)
            (self.fixture.root / 'release.toml').write_text(text)
            with self.assertRaisesRegex(ptr.PackagingError, 'packager owns'):
                self.fixture.config()

    def test_unknown_key_is_an_error(self):
        (self.fixture.root / 'release.toml').write_text(
            CONFIG.format(extra='').replace('[build]', '[build]\nskip_audit = true'))
        with self.assertRaisesRegex(ptr.PackagingError, 'unknown key'):
            self.fixture.config()

    def test_missing_payload_file_is_an_error(self):
        (self.fixture.root / 'README.md').unlink()
        with self.assertRaisesRegex(ptr.PackagingError, 'not found'):
            self.fixture.config()

    def test_package_path_cannot_escape(self):
        (self.fixture.root / 'release.toml').write_text(
            CONFIG.format(extra='').replace('to = "input.ini"', 'to = "../input.ini"'))
        with self.assertRaisesRegex(ptr.PackagingError, 'relative path'):
            self.fixture.config()


class NamingTests(unittest.TestCase):
    def test_artifact_names_follow_channel(self):
        fixture = Fixture()
        self.addCleanup(fixture.close)
        config = fixture.config()
        self.assertEqual(ptr.artifact_name(config, 'v1.2.3', 'public', 'windows'),
                         'FixtureRecomp-v1.2.3-windows-x64.zip')
        self.assertEqual(ptr.artifact_name(config, '1.2.3', 'playtest', 'windows'),
                         'FixtureRecomp-v1.2.3-playtest-windows-x64.zip')
        self.assertEqual(ptr.artifact_name(config, '0.2.4-alpha', 'playtest', 'linux'),
                         'FixtureRecomp-v0.2.4-alpha-playtest-linux-x86_64.AppImage')


class GateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.stage = Path(self.tmp.name) / 'stage'
        write_stage(self.stage, 'public')

    def test_clean_stage_passes(self):
        ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def assert_refused(self, rel, data=b'x', message=None):
        path = self.stage / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        with self.assertRaises(ptr.PackagingError) as raised:
            ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')
        if message:
            self.assertIn(message, str(raised.exception))
        top = self.stage / Path(rel).parts[0]
        if top.is_dir() and top.name not in ('bios', 'mods'):
            shutil.rmtree(top)
        else:
            path.unlink()

    def test_forbidden_content(self):
        self.assert_refused('Game (USA).cue', message='disc')
        self.assert_refused('bios/SCPH1001.BIN', message='image')
        self.assert_refused('saves/card1.mcd')
        self.assert_refused('settings.toml', message='personal')
        self.assert_refused('mods/installed/x.psxmod', message='personal')
        self.assert_refused('state.pst', message='disc')
        self.assert_refused('lib/game_full_1.c', message='source')
        self.assert_refused('generated/x.h', message='build-kit')

    def test_toolchain_sources_are_allowed(self):
        (self.stage / 'overlay_toolchain/tcc/examples').mkdir(parents=True)
        (self.stage / 'overlay_toolchain/tcc/examples/fib.c').write_text('int x;')
        ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def test_hash_bound_disc_overlay_asset(self):
        package = self.stage / 'mods/bundled/fixture.patch/1.0.0'
        (package / 'assets').mkdir(parents=True)
        asset = package / 'assets/warning.bin'
        asset.write_bytes(b'authored patch fixture')
        (package / 'manifest.toml').write_text(
            '[[overlay]]\ntarget="disc_user"\noffset=16\n'
            'file="assets/warning.bin"\nsha256="%s"\nexpected_sha256="%s"\n'
            % (hashlib.sha256(asset.read_bytes()).hexdigest(), '0' * 64))
        ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')
        asset.write_bytes(b'tampered')
        with self.assertRaisesRegex(ptr.PackagingError, 'hash mismatch'):
            ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def test_undeclared_mod_binary_is_refused(self):
        self.assert_refused('mods/bundled/fixture/1.0.0/assets/disc.bin', message='image')

    def test_openbios_required(self):
        (self.stage / 'bios/openbios.bin').write_bytes(b'short')
        with self.assertRaisesRegex(ptr.PackagingError, 'OpenBIOS'):
            ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def test_windows_binary_in_linux_package(self):
        (self.stage / 'Fixture').write_bytes(b'\x7fELF')
        (self.stage / 'extra.dll').write_bytes(b'MZ')
        with self.assertRaisesRegex(ptr.PackagingError, 'Windows binary'):
            ptr.payload_gates(self.stage, 'linux', 'Fixture')

    def test_template_tokens_must_all_be_stamped(self):
        template = Path(self.tmp.name) / 'AppRun'
        template.write_text('echo @VERSION@ @UNKNOWN_TOKEN@\n')
        with self.assertRaisesRegex(ptr.PackagingError, 'UNKNOWN_TOKEN'):
            ptr.render_template(template, {'VERSION': '1'})
        template.write_text('echo @VERSION@\n')
        self.assertEqual(ptr.render_template(template, {'VERSION': '1'}), 'echo 1\n')


class DiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def ctx(self, channel):
        ctx = ptr.Context()
        ctx.config, ctx.channel = self.fixture.config(), channel
        return ctx

    def test_defaults_follow_channel(self):
        for staged, channel, ok in (('public', 'public', True), ('playtest', 'playtest', True),
                                    ('playtest', 'public', False), ('public', 'playtest', False)):
            stage = Path(self.tmp.name) / ('%s-%s' % (staged, channel))
            write_stage(stage, staged)
            if ok:
                ptr.check_diagnostics(self.ctx(channel), stage)
            else:
                with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                    ptr.check_diagnostics(self.ctx(channel), stage)

    def test_undeclared_diagnostics_package_fails(self):
        (self.fixture.root / 'release.toml').write_text(
            CONFIG.format(extra='').replace(
                '[diagnostics]\ngame_id = "SLUS-00000"\npackage_id = "fixture.diagnostics"\n', ''))
        stage = Path(self.tmp.name) / 'stage'
        write_stage(stage, 'public')
        with self.assertRaisesRegex(ptr.PackagingError, 'no \\[diagnostics\\]'):
            ptr.check_diagnostics(self.ctx('public'), stage)


class VerifyZipTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        self.config = self.fixture.config()
        self.out = Path(self.fixture.root) / 'dist'
        self.out.mkdir()

    def build(self, channel, staged_channel=None):
        stage = Path(self.fixture.root) / ('stage-' + channel)
        write_stage(stage, staged_channel or channel)
        artifact = self.out / ptr.artifact_name(self.config, 'v1.2.3', channel, 'windows')
        zip_stage(stage, artifact)
        return artifact

    def test_public_and_playtest_verify(self):
        for channel in ptr.CHANNELS:
            manifest = ptr.verify_artifact(self.build(channel), self.config, channel, 'v1.2.3')
            self.assertEqual(manifest['channel'], channel)

    def test_wrong_channel_fails(self):
        artifact = self.build('public')
        with self.assertRaises(ptr.PackagingError):
            ptr.verify_artifact(artifact, self.config, 'playtest', 'v1.2.3')

    def test_tampered_payload_fails(self):
        artifact = self.build('public')
        with zipfile.ZipFile(artifact, 'a') as archive:
            archive.writestr('saves/card.mcd', b'card')
        Path(str(artifact) + '.sha256').write_text(
            '%s  %s\n' % (ptr.sha256(artifact), artifact.name))
        with self.assertRaises(ptr.PackagingError):
            ptr.verify_artifact(artifact, self.config, 'public', 'v1.2.3')

    def test_stale_checksum_fails(self):
        artifact = self.build('public')
        Path(str(artifact) + '.sha256').write_text('0' * 64 + '  x\n')
        with self.assertRaisesRegex(ptr.PackagingError, 'sha256'):
            ptr.verify_artifact(artifact, self.config, 'public', 'v1.2.3')


if __name__ == '__main__':
    unittest.main()
