"""A parked deferred load is committed before psx_syscall runs (PS1B-529).

Builds the fixture against the production traps.c, at -O0 and -O2. The fixture
is the guest; fixture_link stubs any symbol the syscall path does not reach.

Usage: python test_syscall_load_value_commit.py [--cc gcc] [--src-root DIR]
"""
import argparse
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fixture_link import build_and_run

GUEST = 'test_syscall_load_value_commit.c'
MODULES = ['traps.c']

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--src-root', default=None,
                        help='the runtime directory whose src/ and include/ are built')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    src_root = Path(args.src_root).resolve() if args.src_root else here.parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            work = Path(root) / ('work' + opt)
            work.mkdir()
            output = build_and_run(args.cc, here, src_root, opt, work, MODULES, GUEST)
            print(opt, output.rstrip(), flush=True)
    print('PASS: psx_syscall commits a parked load before it reads or writes registers (O0/O2)')
