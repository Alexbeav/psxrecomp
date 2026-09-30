"""ChangeThread to the running thread must continue after the SYSCALL (PS1G-58)."""
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
                          ['traps.c'], 'test_syscall_change_thread_self.c')
    print('PASS: same-thread ChangeThread continues after SYSCALL (O0/O2)')
