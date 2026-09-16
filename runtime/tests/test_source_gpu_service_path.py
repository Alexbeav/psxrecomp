"""Exercise the real service continuation after command update, at O0 and O2."""
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
                          ['source_gpu_runtime.c'], 'test_source_gpu_service_path.c')
    print('PASS: deferred GPU draw, raster advance and DMA service (O0/O2)')
