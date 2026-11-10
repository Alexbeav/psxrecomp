"""A completed load survives the production interrupt return path."""
import argparse
import tempfile
from pathlib import Path
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
                          'test_irq_pending_load.c')
    print('PASS: pending load survives same-thread interrupt return (O0/O2)')
