"""Exercise issue774 against the maintained real-poller and anchor fixture."""
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
    print('noop: serial O0/O2 real-poller scope checks; fake serializers, no slot files or devices')
    raise SystemExit(0)
core = Path(__file__).resolve().parents[1]
lane = core.parent
config = json.loads((lane/'reports/consumer-790-config.json').read_text(encoding='utf-8'))
compiler = Path(next(a.split('=', 1)[1] for a in config['configure_args'] if a.startswith('-DCMAKE_C_COMPILER=')))
output = lane.parent/'PS1B-165-pair'/f'state-defer-{args.attempt}'
output.mkdir()
sys.path.insert(0, str(core/'tools/public_studio'))
from backend import Runner
runner = Runner(log=lambda line: print(line, flush=True))
environment = os.environ.copy()
environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
for optimization in ('-O0', '-O2'):
    binary = output/(optimization[1:]+'.exe')
    runner.run([compiler, optimization, '-std=c11', '-I', core/'runtime/include',
                core/'runtime/tests/test_savestate_title_policy.c', core/'runtime/src/mod_netplay.c',
                core/'runtime/src/psx_sha256.c', '-o', binary], output, env=environment)
    runner.run([binary], output, env=environment)
print('STATE_DEFER_POLICY_OPTIMIZATIONS_PASS=2', flush=True)
