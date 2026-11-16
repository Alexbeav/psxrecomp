"""Exercise the real service continuation after command update, at O0 and O2."""
import argparse
from pathlib import Path
import tempfile
import os
import subprocess
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
            exe = Path(root) / ('sectionwire' + opt.replace('-', '') + ('.exe' if os.name == 'nt' else ''))
            env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PSX_')}
            for terminal, expected in ((0, 0), (6, 2), (7, 0), (8, 2)):
                run = subprocess.run([str(exe), str(terminal)], env=env, capture_output=True,
                                     text=True, encoding='utf-8', errors='replace', timeout=60)
                assert run.returncode == expected, (opt, terminal, run.returncode, run.stderr)
    print('PASS: deferred GPU draw, raster advance and DMA service (O0/O2)')
