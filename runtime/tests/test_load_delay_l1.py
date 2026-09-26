"""Run authored L1 instructions through the real decoder; unexpected seams abort."""
import argparse
from pathlib import Path
import tempfile
from source_fixture_link import build_and_run
import source_fixture_link

def named_stubs(path, symbols):
    path.write_text('#include <stdio.h>\n#include <stdlib.h>\n' + '\n'.join(
        f'void {s}(void) {{ fprintf(stderr,"unexpected seam: {s}\\n"); exit(99); }}'
        for s in sorted(symbols)))
source_fixture_link.write_stubs = named_stubs

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default='gcc')
    args = ap.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as tmp:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(tmp),
                          [str(here / 'load_delay_decoder.c')], 'test_load_delay_l1.c')
    print('PASS: L1 production decoder O0/O2')
