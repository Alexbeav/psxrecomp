"""Compile the actual persisted/CLI adapters; test relocation without game data."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    text = (root / 'src/main.cpp').read_text(encoding='utf-8')
    adapters = []
    for name in ('normalize_disc_path_for_launch', 'resolve_persisted_disc_path'):
        start = text.index('static std::filesystem::path ' + name + '(')
        adapters.append(text[start:text.index('\n}\n', start) + 2])
    assert 'resolved_disc = resolve_persisted_disc_path(us.disc_path, exe_dir_from_argv(argv[0]))' in text
    assert 'cached = resolve_persisted_disc_path(cached, exe_dir_from_argv(argv0))' in text
    with tempfile.TemporaryDirectory(prefix='persisted disc spaces ') as tmp:
        base = Path(tmp)
        product, cwd = base / 'moved product', base / 'unrelated cwd'
        (product / 'inputs').mkdir(parents=True)
        (cwd / 'inputs').mkdir(parents=True)
        relative = 'inputs/Resident Evil 2 [Leon] [SLUS-00748].chd'
        (product / relative).write_bytes(b'authored placeholder')
        (cwd / relative).write_bytes(b'wrong cwd decoy')
        source = base / 'probe.cpp'
        source.write_text('''#include <filesystem>
#include <iostream>
#include "host_path.h"
#include "disc_path.h"
''' + '\n'.join(adapters) + '''
int main(int argc, char** argv) {
    const std::filesystem::path path = argv[2];
    const auto resolved = std::string(argv[1]) == "persisted"
        ? resolve_persisted_disc_path(path, argv[3])
        : normalize_disc_path_for_launch(path);
    // libstdc++'s generic_string() collapses a UNC "\\server" root to "/server";
    // print the native spelling with forward slashes so both C++ libraries agree.
    std::string out = resolved.string();
    for (char& c : out) if (c == '\\\\') c = '/';
    std::cout << out;
}
''', encoding='utf-8')
        compiler = shutil.which(args.compiler) or args.compiler
        env = os.environ.copy()
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        exe = base / ('probe.exe' if os.name == 'nt' else 'probe')
        subprocess.run([compiler, '-std=c++17', '-Werror=return-type', '-I'+str(root/'include'),
                        '-I'+str(root.parent/'recompiler/include'), str(source),
                        str(root/'src/disc_path.cpp'), str(root/'src/cue_sheet.cpp'), '-o', str(exe)],
                       check=True, env=env)
        def check(mode, path, expected):
            result = subprocess.run([str(exe), mode, str(path), str(product)], cwd=cwd,
                                    env=env, check=True, capture_output=True, text=True)
            assert result.stdout == expected, (result.stdout, expected)
        check('persisted', relative, (product/relative).as_posix())
        check('persisted', product/relative, (product/relative).as_posix())
        check('persisted', '', '')
        check('persisted', 'inputs/missing.chd', (product/'inputs/missing.chd').as_posix())
        check('cli', relative, (cwd/relative).as_posix())
        if os.name == 'nt':
            check('persisted', r'\\server\share\RE2 [Leon].chd', '//server/share/RE2 [Leon].chd')
        print('persisted disc: relative, absolute, empty, missing, CLI cwd and Windows UNC passed')


if __name__ == '__main__':
    main()
