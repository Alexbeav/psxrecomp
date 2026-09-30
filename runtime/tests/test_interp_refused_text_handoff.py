"""PS1B-97: straight-line interpretation does not hand refused game text back."""
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
                          ['dirty_ram_interp.c'], 'test_interp_refused_text_handoff.c',
                          defines=('PSX_HAS_GAME_DISPATCH', 'PSX_NO_DEBUG_TOOLS'))
    print('PASS: interpretation runs through refused game text (O0/O2)')
