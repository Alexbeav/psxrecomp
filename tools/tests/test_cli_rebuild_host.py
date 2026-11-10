"""psxrecomp_cli.py rebuild: host portability and configure-flag precedence.

No game or CMake build runs: product configure/build are recorders. The clean
exit timer is compiled alone with synthetic SDL ticks and no frontend.
"""
import os
import shutil
import subprocess
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tools')]
import psxrecomp_cli as cli  # noqa: E402


class ClampFutureMtimesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.future = time.time() + 3600

    def tearDown(self):
        self.tmp.cleanup()

    def touch_future(self, rel):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('x')
        os.utime(path, (self.future, self.future))
        return path

    def test_clamps_without_lutimes(self):
        # Windows CPython: os.utime is absent from supports_follow_symlinks and
        # follow_symlinks=False raises NotImplementedError, not OSError.
        log = self.touch_future('build-release.log')
        with mock.patch.object(cli, '_UTIME_NOFOLLOW', False):
            self.assertEqual(cli.clamp_future_mtimes(self.root), 1)
        self.assertLessEqual(log.stat().st_mtime, time.time())

    def test_skips_build_dir(self):
        self.touch_future('build-release/out.o')
        src = self.touch_future('src/a.c')
        self.assertEqual(
            cli.clamp_future_mtimes(self.root, skip=self.root / 'build-release'), 1)
        self.assertLessEqual(src.stat().st_mtime, time.time())

    def test_never_retimes_symlink_target_without_lutimes(self):
        outside = tempfile.TemporaryDirectory()
        self.addCleanup(outside.cleanup)
        target = Path(outside.name) / 'target.txt'
        target.write_text('x')
        os.utime(target, (self.future, self.future))
        try:
            os.symlink(target, self.root / 'link.txt')
        except (OSError, NotImplementedError):
            self.skipTest('symlinks unavailable on this host')
        with mock.patch.object(cli, '_UTIME_NOFOLLOW', False):
            self.assertEqual(cli.clamp_future_mtimes(self.root), 0)
        self.assertGreater(target.stat().st_mtime, time.time())


class RebuildConfigureFlagsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'game.toml').write_text('[game]\nid = "TEST-00001"\n')
        self.configures = []

    def tearDown(self):
        self.tmp.cleanup()

    def rebuild(self, extra, pgo=False):
        args = cli.build_parser().parse_args(
            ['rebuild', '--config', str(self.root / 'game.toml'),
             '--project-root', str(self.root), '--build-dir', 'build-x',
             *(['--force-pgo', '--disc', str(self.root / 'game.toml')] if pgo
               else ['--no-pgo']),
             *[f'--cmake-extra={e}' for e in extra]])
        exe = self.root / 'build-x' / 'psx-runtime.exe'

        def configure(root, build_dir, pgo, extra, progress):
            self.configures.append((pgo, list(extra)))
            Path(build_dir).mkdir(parents=True, exist_ok=True)
            (Path(build_dir) / 'CMakeCache.txt').write_text('\n'.join(
                flag[2:].replace('=', ':BOOL=', 1)
                for flag in extra if flag.startswith('-D') and '=' in flag), encoding='utf-8')

        with mock.patch.object(cli, 'activate_embedded_toolchain', return_value=True), \
             mock.patch.object(cli, '_cmake_configure', side_effect=configure), \
             mock.patch.object(cli, '_cmake_build'), \
             mock.patch.object(cli, 'run_pgo_train'), \
             mock.patch.object(cli, '_resolve_runtime_exe', return_value=(exe, '')), \
             mock.patch.object(cli, 'stage_overlay_toolchain_for_product'):
            self.assertEqual(cli.cmd_rebuild(args, mock.Mock()), cli.EXIT_OK)

    @staticmethod
    def debug_tools(extra):
        values = [e.split('=', 1)[1] for e in extra if e.startswith('-DPSX_DEBUG_TOOLS=')]
        return values[-1]  # CMake applies the last -D for a name

    def test_shipping_build_defaults_debug_tools_off(self):
        self.rebuild([])
        self.assertEqual([self.debug_tools(e) for _, e in self.configures], ['OFF'])

    def test_cmake_extra_overrides_debug_tools(self):
        self.rebuild(['-DPSX_DEBUG_TOOLS=ON'])
        self.assertEqual([self.debug_tools(e) for _, e in self.configures], ['ON'])

    def test_pgo_instrument_stage_uses_the_product_shape(self):
        # The runtime's clean self-exit flushes profiles without a debug server.
        # Instrumented and final functions must have the same debug control flow.
        self.rebuild(['-DPSX_DEBUG_TOOLS=OFF'], pgo=True)
        self.assertEqual([(p, self.debug_tools(e)) for p, e in self.configures],
                         [('generate', 'OFF'), ('use', 'OFF')])


class ProductExitTimerTest(unittest.TestCase):
    def test_the_actual_runtime_timer_is_disabled_or_exits_cleanly_at_its_deadline(self):
        compiler = shutil.which(os.environ.get('CXX', 'g++'))
        if not compiler:
            self.skipTest('C++ compiler unavailable for the isolated runtime timer')
        text = (ROOT / 'runtime/src/main.cpp').read_text(encoding='utf-8')
        start = text.index('static void exit_after_tick(void) {')
        body = text[start:text.index('\nstatic void frame_report_tick', start)]
        self.assertIn('exit_after_tick();', text[text.index('frame_report_tick(s_frame_count);'):])
        harness = '''#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cassert>
static uint64_t ticks;
uint64_t SDL_GetTicks() { return ticks; }
void psx_crash_trace_set_exit_origin(const char *origin) {
    assert(!std::strcmp(origin,"exit_after_ms"));
    std::puts("clean-exit-origin");
}
'''+body+'''
int main(int argc,char **argv) {
    assert(argc==2); ticks=100; exit_after_tick();
    ticks=149; exit_after_tick(); std::puts("before-deadline");
    ticks=150; exit_after_tick(); std::puts("timer-disabled");
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            src = root / 'timer.cpp'
            src.write_text(harness, encoding='utf-8')
            exe = root / ('timer.exe' if os.name == 'nt' else 'timer')
            for opt in ('-O0', '-O2'):
                subprocess.run([compiler, '-std=c++17', opt, str(src), '-o', str(exe)],
                               stdin=subprocess.DEVNULL, check=True, capture_output=True)
                for value in ('0', '-5', '50'):
                    env = dict(os.environ, PSX_EXIT_AFTER_MS=value)
                    result = subprocess.run([str(exe), 'synthetic'], env=env,
                                            stdin=subprocess.DEVNULL, capture_output=True,
                                            text=True, encoding='utf-8', errors='replace', timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('before-deadline', result.stdout)
                    self.assertEqual('clean-exit-origin' in result.stdout, value == '50')
                    self.assertEqual('timer-disabled' in result.stdout, value != '50')


class CmakeBuildToolTest(unittest.TestCase):
    """Build must run the cmake that configured the tree, never a PATH cmake.

    On Windows a PATH ``cmake`` is often MSYS2's, which rewrites the Windows
    build directory as ``/f/.../F:/...`` and fails.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.build = Path(self.tmp.name) / 'build-release'
        self.build.mkdir()
        self.configured = Path(self.tmp.name) / 'toolchain' / 'bin' / 'cmake.exe'
        self.configured.parent.mkdir(parents=True)
        self.configured.write_text('')

    def tearDown(self):
        self.tmp.cleanup()

    def build_cmd(self):
        calls = []

        def run(cmd, **_):
            calls.append(cmd)
            return mock.Mock(returncode=0, stdout='', stderr='')

        with mock.patch.object(cli.subprocess, 'run', side_effect=run):
            cli._cmake_build(self.build, 'psx-runtime', mock.Mock())
        return calls[0]

    def test_uses_cmake_command_from_cache(self):
        (self.build / 'CMakeCache.txt').write_text(
            'CMAKE_GENERATOR:INTERNAL=Ninja\n'
            f'CMAKE_COMMAND:INTERNAL={self.configured}\n')
        with mock.patch.object(cli, '_which_tool', return_value=Path('/msys/usr/bin/cmake')):
            self.assertEqual(self.build_cmd()[0], str(self.configured))

    def test_falls_back_to_path_without_cache(self):
        with mock.patch.object(cli, '_which_tool', return_value=Path('/usr/bin/cmake')):
            self.assertEqual(self.build_cmd()[0], str(Path('/usr/bin/cmake')))

    def test_stale_cache_entry_falls_back_to_path(self):
        (self.build / 'CMakeCache.txt').write_text(
            f'CMAKE_COMMAND:INTERNAL={self.configured.parent / "gone.exe"}\n')
        with mock.patch.object(cli, '_which_tool', return_value=Path('/usr/bin/cmake')):
            self.assertEqual(self.build_cmd()[0], str(Path('/usr/bin/cmake')))

    def test_no_cmake_anywhere_is_an_error(self):
        with mock.patch.object(cli, '_which_tool', return_value=None):
            with self.assertRaises(RuntimeError):
                cli._cmake_build(self.build, 'psx-runtime', mock.Mock())


if __name__ == '__main__':
    unittest.main()
