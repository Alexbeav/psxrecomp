"""CPU register and timing wire regression, without retail assets."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--cc', default='gcc')
a = p.parse_args()
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as work:
    exe = Path(work) / 'cpu-wire.exe'
    for opt in ('-O0', '-O2'):
        subprocess.run([a.cc, '-std=c11', opt, '-Wall', '-Wextra', '-Werror',
                        '-I', str(here.parent / 'include'),
                        str(here / 'test_cpu_state_wire.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('PASS: CPU timing sentinels, invalid indexes, old wire refusal (O0/O2)')
