"""Authored fallback controls; builds/discs/BIOS are mocked except the isolated C reader."""
import argparse
from contextlib import ExitStack
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import release_stage
from setup_degrades import begin_degrades, record_degrade, finish_degrades


def load_cli(path):
    spec = importlib.util.spec_from_file_location('_cli_degrades_under_test', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


cli = load_cli(ROOT / 'psxrecomp_cli.py')


class DegradeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='psx-degrade-controls-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.progress = Mock()

    def begin(self, operation='rebuild'):
        begin_degrades(self.progress, self.root, operation)

    def codes(self):
        return [row['code'] for row in self.progress._setup_degrades.rows]

    def rebuild(self, *, portable=True, pgo='', diagnostic=False, plain_fails=False,
                zip_arg='', no_cmake=False):
        config = self.root / 'game.toml'
        config.write_text('[game]\nname="Authored"\ndisc="authored.cue"\n', encoding='utf-8')
        (self.root / 'authored.cue').write_text('authored stand-in; never read as a disc', encoding='utf-8')
        args = argparse.Namespace(config=str(config), project_root=str(self.root), build_dir='normal',
                                  target='authored', exe_basename='Authored', disc='', no_pgo=False,
                                  force_pgo=bool(pgo), no_lto=True, cmake_extra=[], prune_after='',
                                  diagnostic_dir='diagnostic' if diagnostic else '', no_toolchain_download=True,
                                  toolchain_zip=zip_arg)
        def build(folder, *_args):
            if plain_fails:
                raise RuntimeError('authored build failure')
            folder = Path(folder)
            folder.mkdir(exist_ok=True)
            (folder / 'Authored.exe').write_bytes(b'MZ')
        with ExitStack() as stack:
            replacements = {
                'activate_embedded_toolchain': Mock(return_value=portable),
                'ensure_toolchain_for_rebuild': Mock(return_value=False),
                '_configure_product': Mock(return_value=False), '_cmake_build': Mock(side_effect=build),
                '_resolve_runtime_exe': Mock(side_effect=lambda d, *_: (Path(d) / 'Authored.exe', None)),
                'stage_overlay_toolchain_for_product': Mock(), 'stage_notices_for_product': Mock(),
                'pgo_merge_tool_available': Mock(return_value=None if pgo == 'unavailable' else 'authored'),
                'run_pgo_train': Mock(side_effect=RuntimeError('authored train failure')),
                'build_diagnostic_product': Mock(return_value=(None, 'authored diagnostic failure')),
            }
            for name, replacement in replacements.items():
                stack.enter_context(patch.object(cli, name, replacement))
            stack.enter_context(patch.object(cli.shutil, 'which', return_value=None if no_cmake else 'authored-cmake'))
            code = cli.cmd_rebuild(args, self.progress)
        return code, self.progress.result.call_args.kwargs if self.progress.result.called else None

    def test_actual_rebuild_result_reports_system_fallback(self):
        code, result = self.rebuild(portable=False)
        self.assertEqual(code, cli.EXIT_OK)
        self.assertIn('toolchain.system_fallback', [row['code'] for row in result.get('degrades', [])])
        self.assertTrue((self.root / '.cache/setup-degrades-rebuild.txt').is_file())

    def test_actual_rebuild_healthy_pgo_and_diagnostic_controls(self):
        for route, expected in (('', None), ('unavailable', 'pgo.unavailable'), ('failed', 'pgo.failed')):
            with self.subTest(route=route):
                self.progress = Mock()
                code, result = self.rebuild(pgo=route)
                self.assertEqual(code, cli.EXIT_OK)
                codes = [row['code'] for row in result['degrades']]
                self.assertEqual(codes, [expected] if expected else [])
                self.assertEqual(bool(result['pgo_skipped']), bool(expected))
        self.progress = Mock()
        code, result = self.rebuild(diagnostic=True)
        self.assertEqual(code, cli.EXIT_OK)
        self.assertTrue(result['ok'])
        self.assertTrue(Path(result['exe']).is_file())
        self.assertIn('diagnostic.optional_failure', [row['code'] for row in result['degrades']])
        self.progress = Mock()
        code, result = self.rebuild(pgo='failed', plain_fails=True)
        self.assertEqual(code, cli.EXIT_ERROR)
        self.assertIsNone(result)

    def test_offline_toolchain_failure_and_missing_system_cmake_remain_fatal(self):
        for parameters in ({'zip_arg': 'authored-missing.zip'}, {'portable': False, 'no_cmake': True}):
            self.progress = Mock()
            code, result = self.rebuild(**parameters)
            self.assertEqual(code, cli.EXIT_ERROR)
            self.assertIsNone(result)

    def test_real_json_progress_exposes_same_fallback_row(self):
        stream = io.StringIO()
        self.progress = cli.ProgressReporter(json_progress=True, stream=stream, log_stream=io.StringIO())
        with patch.object(self.progress, 'result', wraps=self.progress.result):
            code, result = self.rebuild(portable=False)
        self.assertEqual(code, cli.EXIT_OK)
        events = [json.loads(line) for line in stream.getvalue().splitlines()]
        result_row = next(event for event in events if event['event'] == 'result')
        degrade = next(event for event in events if event['event'] == 'degrade')
        self.assertEqual(result_row['degrades'], result['degrades'])
        self.assertEqual(degrade['code'], 'toolchain.system_fallback')
        self.assertEqual(result_row['degrade_report']['scope'], 'last_recorded')

    def test_actual_generate_records_retail_retention_and_preserves_fatal_policy(self):
        config = self.root / 'game.toml'
        config.write_text('[game]\nexe="prepared/BOOT.exe"\n[prepare_disc]\nout_dir="prepared"\n', encoding='utf-8')
        (self.root / 'prepared').mkdir()
        (self.root / 'prepared/BOOT.exe').write_bytes(b'authored boot stand-in')
        (self.root / 'bios.fixture').write_bytes(b'authored firmware stand-in; staging mocked')
        (self.root / 'generated').mkdir()
        (self.root / 'generated/BOOT.exe_dispatch.c').write_text('authored marker', encoding='utf-8')
        args = argparse.Namespace(config=str(config), project_root=str(self.root), disc='missing.cue',
                                  force_prepare=False, skip_hash_check=True, force_bios=True,
                                  bios='bios.fixture', gen_marker='BOOT.exe_dispatch.c')
        for retail, failure, expected in ((True, True, cli.EXIT_OK), (False, True, cli.EXIT_ERROR),
                                          (True, False, cli.EXIT_OK)):
            with self.subTest(retail=retail, failure=failure), ExitStack() as stack:
                self.progress = Mock()
                args.bios = 'bios.fixture' if retail else ''
                for name in ('activate_embedded_toolchain', 'ensure_emitters', 'stage_retail_bios'):
                    stack.enter_context(patch.object(cli, name))
                stack.enter_context(patch.object(cli, 'ensure_framework', return_value=self.root))
                stack.enter_context(patch.object(cli, 'regen_bios_profile',
                                                side_effect=RuntimeError('authored OpenBIOS failure') if failure else None))
                stack.enter_context(patch.object(cli, 'find_emitters', return_value=(Path('authored-game'), Path('authored-bios'))))
                stack.enter_context(patch.object(cli.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')))
                self.assertEqual(cli.cmd_generate(args, self.progress), expected)
                self.assertEqual('bios.openbios_regen' in self.codes(), bool(retail and failure))
                if expected == cli.EXIT_ERROR:
                    self.progress.result.assert_not_called()

    def test_fingerprint_missing_failed_and_healthy_branches(self):
        tools = self.root / 'tools'
        tools.mkdir()
        profile = self.root / 'profile.toml'
        profile.write_text('[recompiler]\nout_stem="Authored"\n', encoding='utf-8')
        script = tools / 'bios_emitter_fingerprint.sh'
        for route in ('no-script', 'no-shell', 'no-profile', 'no-stem', 'failed', 'write-failed', 'healthy'):
            with self.subTest(route=route), ExitStack() as stack:
                self.begin()
                if route != 'no-script':
                    script.write_text('authored script; never executed', encoding='utf-8')
                profile.write_text('[recompiler]\n' + ('' if route == 'no-stem' else 'out_stem="Authored"\n'), encoding='utf-8')
                stack.enter_context(patch.object(cli.shutil, 'which', return_value=None if route == 'no-shell' else 'authored-bash'))
                stack.enter_context(patch.object(cli.subprocess, 'run', return_value=SimpleNamespace(
                    returncode=1 if route == 'failed' else 0, stdout='authored-stamp', stderr='')))
                if route == 'write-failed':
                    original = Path.write_text
                    def write(path, *args, **kwargs):
                        if path.suffix == '.sha':
                            raise OSError('authored stamp write failure')
                        return original(path, *args, **kwargs)
                    stack.enter_context(patch.object(Path, 'write_text', write))
                cli.write_bios_emitter_stamp(self.root, 'missing.toml' if route == 'no-profile' else 'profile.toml', progress=self.progress)
                self.assertEqual('bios.emitter_stamp' in self.codes(), route != 'healthy')

    def test_lto_fallbacks_and_explicit_off(self):
        for platform, memory, disabled, expected in (('darwin', 8, False, True), ('darwin', 16, False, False),
                                                    ('win32', 8, False, False), ('darwin', 8, True, False)):
            self.begin()
            with patch.object(cli.sys, 'platform', platform), patch.object(cli, '_host_memory_gb', return_value=memory):
                cli.product_lto_enabled(SimpleNamespace(no_lto=disabled), self.progress)
            self.assertEqual('lto.low_memory' in self.codes(), expected)
        for unsupported in (True, False):
            self.begin()
            error = RuntimeError('authored configure failure')
            error.cmake_output = 'IPO is unsupported' if unsupported else 'unrelated configure error'
            with patch.object(cli, '_cmake_configure', side_effect=[error, None]), patch.object(cli, '_assert_configured'):
                if unsupported:
                    self.assertFalse(cli._configure_product(self.root, self.root / 'normal', pgo='',
                                                           cmake_extra=[], lto=True, progress=self.progress))
                else:
                    with self.assertRaises(RuntimeError):
                        cli._configure_product(self.root, self.root / 'normal', pgo='', cmake_extra=[], lto=True, progress=self.progress)
            self.assertEqual('lto.unsupported' in self.codes(), unsupported)

    def test_overlay_staging_and_compiler_tiers_for_both_products(self):
        (self.root / 'bin').mkdir()
        (self.root / 'bin/clang.exe').write_bytes(b'')
        for product in ('normal', 'diagnostic'):
            for route in ('failed', 'tcc', 'native'):
                self.begin()
                exe_dir = self.root / product
                tk = exe_dir / 'overlay_toolchain'
                (tk / 'include').mkdir(parents=True, exist_ok=True)
                with patch.object(cli, 'framework_root', return_value=self.root), \
                        patch.object(cli, 'find_psxrecomp_game', return_value=self.root / 'game.exe'), \
                        patch.object(cli.sys, 'platform', 'win32'), \
                        patch.object(cli, 'resolve_toolchain_bin', return_value=self.root / 'bin' if route == 'native' else None), \
                        patch.object(release_stage, 'stage_toolchain', side_effect=OSError('authored staging failure') if route == 'failed' else None,
                                     return_value=str(tk)):
                    result = cli.stage_overlay_toolchain_for_product(self.root, exe_dir, self.progress)
                self.assertEqual(result is None, route == 'failed')
                self.assertEqual(self.codes(), ['overlay.staging'] if route == 'failed' else ['overlay.tcc_tier'] if route == 'tcc' else [])

    def test_notice_partial_missing_and_healthy_measurements_for_both_products(self):
        framework = self.root / 'framework'
        framework.mkdir()
        (self.root / 'LICENSE').write_text('authored project notice', encoding='utf-8')
        (framework / 'LICENSE').write_text('authored framework notice', encoding='utf-8')
        carried = self.root / 'licenses/toolchain'
        carried.mkdir(parents=True)
        for product in ('normal', 'diagnostic'):
            for route in ('partial', 'missing', 'healthy'):
                self.begin()
                notice = carried / 'LICENSE.txt'
                if route == 'missing':
                    notice.unlink(missing_ok=True)
                else:
                    notice.write_text('authored carried notice', encoding='utf-8')
                with ExitStack() as stack:
                    stack.enter_context(patch.object(cli, 'framework_root', return_value=framework))
                    if route == 'partial':
                        stack.enter_context(patch.object(release_stage, '_copy_notices', side_effect=[1, OSError('authored partial copy')]))
                    counts = cli.stage_notices_for_product(self.root, self.root / product, self.progress)
                self.assertEqual('notices.partial_or_missing' in self.codes(), route != 'healthy')
                if route == 'partial':
                    self.assertEqual(counts['kit'], 1)  # Nonempty partial counts are not completion.

    def test_timestamp_cleanup_and_track_list_policies(self):
        source = self.root / 'authored.txt'
        source.write_text('authored', encoding='utf-8')
        self.begin()
        os.utime(source, (10, 10))
        with patch.object(cli.os, 'utime', side_effect=OSError('authored time failure')):
            self.assertEqual(cli.clamp_future_mtimes(self.root, now=1, progress=self.progress), 0)
        self.assertIn('mtime.clamp', self.codes())
        self.begin()
        self.assertGreater(cli.clamp_future_mtimes(self.root, now=1, progress=self.progress), 0)
        self.assertNotIn('mtime.clamp', self.codes())
        build = self.root / 'normal'
        build.mkdir()
        (build / 'CMakeCache.txt').write_text('authored', encoding='utf-8')
        self.begin()
        original = Path.unlink
        def unlink(path, *args, **kwargs):
            if path.name == 'CMakeCache.txt':
                raise OSError('authored cleanup refusal')
            return original(path, *args, **kwargs)
        with patch.object(Path, 'unlink', unlink):
            cli.prune_after_rebuild(self.root, build, {'build-intermediates'}, self.progress)
        self.assertIn('cleanup.incomplete', self.codes())
        for status, policy, expected in (('not_checked', 'warn', True), ('mismatch', 'warn', True),
                                          ('match', 'warn', False), ('mismatch', cli.disc_track_list.REFUSE, False)):
            self.begin()
            row = {'status': status, 'policy': policy, 'sentence': 'authored track-list result'}
            with patch.object(cli.disc_track_list, 'check', return_value=row):
                if policy == cli.disc_track_list.REFUSE:
                    with self.assertRaises(cli.DiscVerifyError):
                        cli.check_track_list({}, source, {}, {}, self.progress, skip_hash=False, data_track_checked=True)
                else:
                    cli.check_track_list({}, source, {}, {}, self.progress, skip_hash=False, data_track_checked=True)
            self.assertEqual('disc.track_list' in self.codes(), expected)

    def test_report_persistence_failure_is_visible_in_cli_result(self):
        self.begin('generate')
        record_degrade(self.progress, 'bios.emitter_stamp', 'authored reason')
        self.begin('rebuild')
        record_degrade(self.progress, 'pgo.unavailable', 'authored reason')
        with patch('setup_degrades.os.replace', side_effect=OSError('authored persistence refusal')):
            result = finish_degrades(self.progress)
        self.assertEqual(result['degrade_report']['state'], 'not_saved')
        self.assertIn('report.persistence', [row['code'] for row in result['degrades']])
        self.assertIn('bios.emitter_stamp', (self.root / '.cache/setup-degrades-generate.txt').read_text(encoding='utf-8'))

    def test_native_reader_missing_invalid_incomplete_and_utf8_records(self):
        compiler = os.environ.get('PSX_TEST_C_COMPILER')
        if not compiler:
            self.skipTest('native reader needs explicitly bound PSX_TEST_C_COMPILER; CTest supplies actual compiler')
        source, exe = self.root / 'reader.c', self.root / ('reader.exe' if os.name == 'nt' else 'reader')
        source.write_text('#include "psx_setup_degrades.h"\n'
                          'int main(int argc,char**argv){PsxSetupDegradeReport r;size_t i;'
                          'if(argc!=2)return 2;psx_setup_degrades_read(argv[1],"rebuild",&r);'
                          'printf("%s\\n%zu\\n",r.state,r.count);'
                          'for(i=0;i<r.count;++i)printf("%s\\t%s\\n",r.rows[i].code,r.rows[i].reason);return 0;}\n', encoding='utf-8')
        if Path(compiler).name.lower() in ('cl.exe', 'clang-cl.exe', 'cl', 'clang-cl'):
            command = [compiler, '/nologo', '/std:c11', '/I' + str(ROOT / 'host'), str(source), '/Fe:' + str(exe)]
        else:
            command = [compiler, '-std=c11', '-I', str(ROOT / 'host'), str(source), '-o', str(exe)]
        built = subprocess.run(command, cwd=self.root, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60,
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        missing = self.root / 'missing.txt'
        def read(path):
            proc = subprocess.run([str(exe), str(path)], cwd=self.root, capture_output=True, text=True,
                                  encoding='utf-8', errors='replace', timeout=10,
                                  creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            return proc.stdout.splitlines()
        self.assertEqual(read(missing), ['missing', '0'])
        self.begin()
        record_degrade(self.progress, 'pgo.unavailable', 'Δ authored path with "quotes"')
        saved = self.root / '.cache/setup-degrades-rebuild.txt'
        self.assertEqual(read(saved), ['recorded_incomplete', '1', 'pgo.unavailable\tΔ authored path with "quotes"'])
        finish_degrades(self.progress)
        valid = saved.read_bytes()
        self.assertEqual(read(saved)[0], 'recorded_complete')
        for data in (b'', b'wrong-schema\n', valid[:-1], valid + b'extra\n',
                     valid.replace(b'\n1\n', b'\n129\n'), valid.replace('Δ'.encode(), b'\xff')):
            saved.write_bytes(data)
            self.assertEqual(read(saved), ['invalid', '0'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-cli', type=Path)
    args, rest = parser.parse_known_args()
    if args.baseline_cli:
        cli = load_cli(args.baseline_cli)
        suite = unittest.TestSuite([DegradeTests('test_actual_rebuild_result_reports_system_fallback')])
    else:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(DegradeTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
