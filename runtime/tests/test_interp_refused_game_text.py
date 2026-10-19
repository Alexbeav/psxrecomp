"""Straight-line interpretation does not hand refused game text back.

Links the production dirty_ram_interp.c (release build, game dispatch on) into
test_interp_refused_game_text.c at -O0 and -O2; every other runtime seam is an
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
                          ['dirty_ram_interp.c'], 'test_interp_refused_game_text.c',
                          defines=('PSX_HAS_GAME_DISPATCH', 'PSX_NO_DEBUG_TOOLS'))
    print('PASS: interpretation runs through refused game text (O0/O2)')
