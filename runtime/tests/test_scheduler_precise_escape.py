"""The scheduler must clear an interpreter owner abandoned by longjmp."""
import argparse
from pathlib import Path
import tempfile
from test_boot_state_section_wire import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['traps.c'], 'test_scheduler_precise_escape.c')
    print('PASS: scheduler resume and exit clear abandoned precise owner (O0/O2)')
