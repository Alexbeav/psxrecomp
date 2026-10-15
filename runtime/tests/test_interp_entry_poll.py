"""PS1B-454 row 5: a deliverable interrupt is taken at the next dirty-RAM
interpreter entry (590c23693, Kula World's CdSync spin)."""
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
                          ['dirty_ram_interp.c'], 'test_interp_entry_poll.c',
                          defines=('PSX_HAS_GAME_DISPATCH', 'PSX_NO_DEBUG_TOOLS'))
    print('PASS: the dirty-RAM entry poll takes a deliverable interrupt (O0/O2)')
