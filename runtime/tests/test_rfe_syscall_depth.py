"""A handler-driven exception return leaves no host frames behind (PS1B-324)."""
import argparse
from pathlib import Path
import tempfile
from source_fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['interrupts.c', 'traps.c', 'psx_bios_backend.c'],
                          'test_rfe_syscall_depth.c')
    print('PASS: handler-driven exception returns keep the host depth flat (O0/O2)')
