"""The setup packaging tools stay runnable on Python 3.8.

A title's Linux release job runs package_setup_host.sh inside ubuntu:20.04, whose
python3 is 3.8. Path.write_text(newline=...) needs 3.10 (Path.read_text(newline=...)
3.13), so one such call stops the setup packaging with a TypeError. This test walks
the tools the setup script starts and the tools/ modules they import. It refuses
syntax newer than 3.8 and those two keywords, and imports each module, which on a
3.8 interpreter also catches annotations such as `int | None` evaluated at import.
The title-release tools (package_title_release.py, aot_overlay_pipeline.py) are not
covered: they do not run in that container.
"""
import ast
import importlib.util
import sys
from pathlib import Path
import unittest

TOOLS = Path(__file__).resolve().parents[1]

# Started by package_setup_host.sh, plus the zip writer the setup release uses.
ENTRY_POINTS = [
    'program_set.py', 'mod_channel_filter.py', 'recomplauncher_manifest.py',
    'release_stage.py', 'setup_host_plain_start.py', 'create_release_zip.py',
]


def local_module(name, here):
    for base in (here.parent, TOOLS):
        for candidate in (base / (name.replace('.', '/') + '.py'),
                          base / name.replace('.', '/') / '__init__.py'):
            if candidate.is_file():
                return candidate
    return None


def release_modules():
    seen, todo = set(), [TOOLS / p for p in ENTRY_POINTS if (TOOLS / p).is_file()]
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        tree = ast.parse(path.read_bytes())
        for node in ast.walk(tree):
            names = []
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                names = [node.module]
            for name in names:
                found = local_module(name, path)
                if found:
                    todo.append(found)
    return sorted(seen)


class ReleaseToolsPy38Test(unittest.TestCase):
    def test_walk_finds_the_release_tools(self):
        names = {p.name for p in release_modules()}
        for expected in ENTRY_POINTS:
            self.assertIn(Path(expected).name, names)

    def test_each_module_imports(self):
        for path in release_modules():
            with self.subTest(path=path.relative_to(TOOLS).as_posix()):
                spec = importlib.util.spec_from_file_location('py38_probe_' + path.stem, path)
                module = importlib.util.module_from_spec(spec)
                sys.path.insert(0, str(path.parent))
                try:
                    spec.loader.exec_module(module)
                finally:
                    sys.path.remove(str(path.parent))

    def test_no_syntax_newer_than_3_8(self):
        for path in release_modules():
            with self.subTest(path=path.relative_to(TOOLS).as_posix()):
                ast.parse(path.read_bytes(), filename=str(path), feature_version=(3, 8))

    def test_no_path_text_newline_keyword(self):
        found = []
        for path in release_modules():
            for node in ast.walk(ast.parse(path.read_bytes())):
                if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                        and node.func.attr in ('write_text', 'read_text')
                        and any(k.arg == 'newline' for k in node.keywords)):
                    found.append('%s:%d' % (path.relative_to(TOOLS).as_posix(), node.lineno))
        self.assertEqual(found, [], 'Path.write_text/read_text newline= needs Python 3.10+; '
                                    'write bytes instead')


if __name__ == '__main__':
    unittest.main()
