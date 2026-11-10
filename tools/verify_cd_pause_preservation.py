"""Run the accepted paused-read position contract against the real controller."""
import argparse
import json
import os
from pathlib import Path
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--attempt', type=int, required=True)
parser.add_argument('--no-op', action='store_true')
args = parser.parse_args()
if args.no_op:
    print('noop: serial O0/O2 synthetic CD controller checks; no media or audio device')
    raise SystemExit(0)
core = Path(__file__).resolve().parents[1]
lane = core.parent
config = json.loads((lane/'reports/consumer-790-config.json').read_text(encoding='utf-8'))
compiler = Path(next(a.split('=', 1)[1] for a in config['configure_args'] if a.startswith('-DCMAKE_C_COMPILER=')))
output = lane.parent/'PS1B-165-pair'/f'cd-pause-{args.attempt}'
output.mkdir()
sys.path.insert(0, str(core/'tools/public_studio'))
from backend import Runner
runner = Runner(log=lambda line: print(line, flush=True))
environment = os.environ.copy()
environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
for optimization in ('-O0', '-O2'):
    binary = output/(optimization[1:]+'.exe')
    runner.run([compiler, optimization, '-std=c11', '-I', core/'runtime/include',
                '-I', core/'runtime/src', core/'runtime/tests/test_cdrom_pause_preservation.c',
                core/'runtime/src/psx_sha256.c', '-o', binary], output, env=environment)
    runner.run([binary], output, env=environment)
print('CD_PAUSE_PRESERVATION_OPTIMIZATIONS_PASS=2', flush=True)
