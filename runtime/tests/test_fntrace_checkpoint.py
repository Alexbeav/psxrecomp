"""Game entry preserves guest RAM; a restored latch skips host handoff actions."""
import argparse
from pathlib import Path
import tempfile
from test_boot_state_section_wire import build_and_run

if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('--cc',default='gcc');a=p.parse_args()
    here=Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as folder:
        for opt in ('-O0','-O2'):
            build_and_run(a.cc,here,here.parent,opt,Path(folder),
                          ['fntrace.c'],'test_fntrace_checkpoint.c')
    print('PASS: game entry preserves guest RAM and restored latch skips handoff (O0/O2)')
