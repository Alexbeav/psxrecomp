"""Exercise the maintained authored source-SPU fixtures through the modern guest clock."""
import argparse
import os
from pathlib import Path
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--attempt', type=int, required=True)
parser.add_argument('--no-op', action='store_true')
parser.add_argument('--include', default='')
args = parser.parse_args()
if args.no_op:
    print('noop: serial source-SPU authored fixtures at O0/O2; no audio device')
    raise SystemExit(0)
core = Path(__file__).resolve().parents[1]
lane = core.parent
compiler = lane/'inputs/toolchain/bin/clang.exe'
output = lane.parent/'PS1B-165-pair'/f'spu-source-{args.attempt}'
output.mkdir()
sys.path.insert(0, str(core/'tools/public_studio'))
from backend import Runner
runner = Runner(log=lambda line: print(line, flush=True))
environment = {key: value for key, value in os.environ.items() if not key.startswith('PSX_')}
environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
names = ('key_latency', 'source_voice_rows', 'source_snapshot', 'register_readback', 's1_block_flags', 'adpcm_sample', 'envelope_fixture')
selected = args.include.split(',') if args.include else list(names)
if not selected or set(selected)-set(names):
    raise SystemExit('Unknown/empty fixture selection: '+args.include)
executed = 0
for optimization in ('-O0', '-O2'):
    for name in selected:
        binary = output/(name+optimization[1:]+'.exe')
        runner.run([compiler, optimization, '-std=c11', '-I', core/'runtime/include',
                    core/f'runtime/tests/test_spu_{name}.c', '-o', binary], output, env=environment)
        extra = [core/'runtime/tests'/f for f in ('spu_envelope_fixture.txt', 'spu_envelope_fixture_e1_e9.txt', 'spu_envelope_fixture_e1r_e10.txt')] if name == 'envelope_fixture' else []
        runner.run([binary, *extra], output, env=environment)
        executed += 1
assert executed == len(selected)*2
print(f'SPU_SOURCE_PROFILE_OPTIMIZATIONS_PASS=2 FIXTURE_EXECUTIONS={executed}', flush=True)