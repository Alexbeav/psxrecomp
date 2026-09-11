"""Atomic state-file replace (overwrite case), compiled at O0/O2 with -Werror.
The helper lives in its own TU (boot_state_replace.c), so this driver compiles
both files and runs the result."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='gcc')
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    root_dir = here.parent
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for opt in ['-O0', '-O2']:
            exe = root / ('replace' + opt + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([a.cc, '-std=c11', opt, '-Wall', '-Wextra', '-Werror',
                            '-I' + str(root_dir / 'include'),
                            str(here / 'test_boot_state_replace.c'),
                            str(root_dir / 'src' / 'boot_state_replace.c'),
                            '-o', str(exe)], check=True)
            workdir = root / ('run' + opt)
            workdir.mkdir()
            env = {k: v for k, v in os.environ.items() if not k.startswith('PSX_')}
            result = subprocess.run([str(exe), str(workdir)], env=env,
                                    capture_output=True, timeout=30)
            assert result.returncode == 0, (opt, result.returncode,
                                            result.stdout, result.stderr)
    print('PASS: atomic replace onto absent and existing target; failure refuses (O0/O2)')

if __name__ == '__main__':
    main()
