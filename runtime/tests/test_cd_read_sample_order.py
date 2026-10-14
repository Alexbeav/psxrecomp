"""Build and run the CD register read sample order fixture without retail assets.

The fixture is test_cd_read_sample_order.c. It compiles the production memory
map and load timing whole and reads no source text. This driver builds it with
the configured C compiler and runs it twice at each optimisation level:

1. As it is. Every case must pass.
2. With the bus wait model switched off (PSX_MMIO_WAIT=0). The default profile
   then samples a CD register before its wait, and the fixture must report
   that. This run shows that the checks can fail.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(exe, env):
    return subprocess.run([str(exe)], env=env, capture_output=True, text=True,
                          encoding='utf-8', errors='replace', timeout=120)


def check(output, cc):
    runtime = Path(__file__).resolve().parents[1]
    source = runtime / 'tests/test_cd_read_sample_order.c'
    env = {key: value for key, value in os.environ.items() if not key.startswith('PSX_')}
    output.mkdir(parents=True, exist_ok=True)
    # -fwhole-program lets GCC drop the devices these loads do not reach. At -O0
    # it keeps every function of memory.c, and the link would need them all.
    for mode in ('O1', 'O2'):
        exe = output / ('cd-read-order-' + mode + '.exe')
        subprocess.run([cc, '-' + mode, '-UNDEBUG', '-fwhole-program',
                        '-DPSX_ENABLE_BLOCK_CYCLES=1', '-DPSX_NO_DEBUG_TOOLS=1',
                        '-I' + str(runtime / 'include'), str(source), '-o', str(exe)], check=True)
        result = run(exe, env)
        assert result.returncode == 0 and result.stdout.startswith('PASS '), \
            (mode, result.returncode, result.stdout[:2000], result.stderr[-2000:])
        print(mode, result.stdout, end='')
        control = run(exe, dict(env, PSX_MMIO_WAIT='0'))
        first = control.stdout.splitlines()[0] if control.stdout else ''
        assert control.returncode == 1 and first.startswith('FAIL default byte status') \
            and 'devices brought up to date' in first, \
            (mode, 'control', control.returncode, control.stdout[:2000], control.stderr[-2000:])
        print(mode, 'control without the bus wait fails as it must:', first)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        check(args.output, args.cc)
    else:
        with tempfile.TemporaryDirectory() as directory:
            check(Path(directory), args.cc)
