"""MFC0/CFC0 read COP0 from before their own cycle charge.

Links the production decoder (interp_cop0_decoder.c, cycle charging on) into
test_interp_cop0_read_sample.c at -O0 and -O2; every other runtime seam is an
abort() stub (fixture_link.py).
"""
import argparse
import tempfile
from pathlib import Path

from fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          [str(here / 'interp_cop0_decoder.c')],
                          'test_interp_cop0_read_sample.c')
    print('PASS: MFC0/CFC0 read CAUSE from before their own cycle charge (O0/O2)')
