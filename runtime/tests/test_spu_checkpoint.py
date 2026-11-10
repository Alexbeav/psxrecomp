"""Restore and consume a wrapped queue of CD audio samples using the real SPU."""
import argparse
from pathlib import Path
import tempfile
from test_boot_state_section_wire import build_and_run

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='gcc')
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as folder:
        for opt in ('-O0', '-O2'):
            build_and_run(a.cc, here, here.parent, opt, Path(folder),
                          [], 'test_spu_checkpoint.c')
    print('PASS: SPU queued CD samples survive restore and ring wrap (O0/O2)')
