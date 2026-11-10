"""Preserve the accepted negative-nibble repair with the actual sanitized decoder."""
import argparse
import os
from pathlib import Path
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--attempt', type=int, required=True)
parser.add_argument('--no-op', action='store_true')
parser.add_argument('--no-sanitize', action='store_true')
parser.add_argument('--source-root', type=Path, help='read decoder, fixture and CRC from this source tree')
args = parser.parse_args()
if args.no_op:
    print('noop: serial actual-decoder C11 shift-sanitizer O0/O2 checks; authored samples, no audio device')
    raise SystemExit(0)
core = Path(__file__).resolve().parents[1]
lane = core.parent
source_root = args.source_root.resolve(strict=True) if args.source_root else core
compiler = lane/'inputs/toolchain/bin/clang.exe'
output = lane.parent/'PS1B-165-pair'/f'spu-nibble-{args.attempt}'
output.mkdir()
sys.path.insert(0, str(core/'tools/public_studio'))
from backend import Runner
runner = Runner(log=lambda line: print(line, flush=True))
environment = {key: value for key, value in os.environ.items() if not key.startswith('PSX_')}
environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=0'
for optimization in ('-O0', '-O2'):
    binary = output/(optimization[1:]+'.exe')
    sanitizer = [] if args.no_sanitize else ['-fsanitize=shift', '-fno-sanitize-recover=shift']
    runner.run([compiler, optimization, '-std=c11', *sanitizer, '-I', source_root/'runtime/include',
                source_root/'runtime/tests/test_spu_signed_nibble.c', source_root/'runtime/src/crc32.c',
                '-o', binary], output, env=environment)
    runner.run([binary], output, env=environment)
print('SPU_SIGNED_NIBBLE_' + ('BASELINE' if args.no_sanitize else 'SANITIZED') + '_OPTIMIZATIONS_PASS=2', flush=True)
