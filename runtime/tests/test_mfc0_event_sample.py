"""PS1B-248: MFC0/CFC0 sample COP0 before their own cycle charge; unexpected seams abort."""
import argparse
import tempfile
from pathlib import Path
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
                          [str(here / 'mfc0_event_decoder.c')], 'test_mfc0_event_sample.c')
