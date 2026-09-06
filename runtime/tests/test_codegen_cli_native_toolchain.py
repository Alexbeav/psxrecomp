"""Exercise the actual CLI tool-selection functions with isolated tool resolvers."""
import ast
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]


def functions(platform):
    names = {'resolve_embedded_toolchain_bin', '_native_toolchain_ready',
             'activate_embedded_toolchain', 'ensure_toolchain_for_rebuild'}
    source = ast.parse((ROOT / 'psxrecomp_cli.py').read_text(encoding='utf-8'))
    body = [ast.ImportFrom(module='__future__', names=[ast.alias(name='annotations')], level=0)]
    body += [n for n in source.body if isinstance(n, ast.FunctionDef) and n.name in names]
    if len(body) != 5:
        raise AssertionError('the four actual CLI functions must exist')
    tree = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    context = {'sys': types.SimpleNamespace(platform=platform), 'Path': Path,
               'shutil': types.SimpleNamespace(which=Mock(side_effect=lambda name: '/usr/bin/' + name)),
               'resolve_toolchain_bin': Mock(return_value=Path('portable/bin')),
               'toolchain_bin_runs': Mock(return_value=True), '_ensure_toolchain_pack': Mock()}
    exec(compile(tree, str(ROOT / 'psxrecomp_cli.py'), 'exec'), context)
    return context


class NativeToolchainTests(unittest.TestCase):
    def test_linux_and_macos_never_resolve_or_download_portable_pack(self):
        for platform in ('linux', 'darwin'):
            context = functions(platform)
            progress = types.SimpleNamespace(log=Mock())
            self.assertIsNone(context['resolve_embedded_toolchain_bin'](Path('.')))
            self.assertTrue(context['activate_embedded_toolchain'](Path('.'), progress))
            self.assertTrue(context['ensure_toolchain_for_rebuild'](Path('.'), progress))
            context['resolve_toolchain_bin'].assert_not_called()
            context['_ensure_toolchain_pack'].assert_not_called()

    def test_missing_native_compiler_fails_before_download(self):
        context = functions('linux')
        context['shutil'].which.side_effect = lambda name: None if name in ('cc', 'gcc', 'clang') else '/usr/bin/' + name
        progress = types.SimpleNamespace(log=Mock())
        self.assertFalse(context['ensure_toolchain_for_rebuild'](Path('.'), progress))
        context['_ensure_toolchain_pack'].assert_not_called()
        self.assertIn('C compiler', progress.log.call_args[0][0])

    def test_windows_keeps_portable_pack_resolution_and_activation(self):
        context = functions('win32')
        activate = Mock()
        module = types.SimpleNamespace(activate_toolchain_bin=activate)
        progress = types.SimpleNamespace(log=Mock())
        with patch.dict(sys.modules, {'toolchain_pack': module}):
            self.assertEqual(context['resolve_embedded_toolchain_bin'](Path('.')), Path('portable/bin'))
            self.assertTrue(context['activate_embedded_toolchain'](Path('.'), progress))
            self.assertTrue(context['ensure_toolchain_for_rebuild'](Path('.'), progress))
        activate.assert_called_once()
        context['_ensure_toolchain_pack'].assert_called_once()
        context['shutil'].which.assert_not_called()


if __name__ == '__main__':
    unittest.main()
