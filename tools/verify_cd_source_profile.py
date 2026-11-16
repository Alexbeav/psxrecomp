"""Exercise actual optional CD clock/device behavior with authored input only."""
import argparse
import json
import os
from pathlib import Path
import struct
import sys

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--attempt',type=int,required=True)
parser.add_argument('--no-op',action='store_true')
parser.add_argument('--include',default='receipt,clock_rows,drive_rows,cdda,snapshot')
args=parser.parse_args()
if args.no_op:
    print('noop: serial authored CD source profile O0/O2, no media/audio device')
    raise SystemExit(0)
core=Path(__file__).resolve().parents[1]
lane=core.parent
config=json.loads((lane/'reports/consumer-790-config.json').read_text(encoding='utf-8'))
compiler=Path(next(a.split('=',1)[1] for a in config['configure_args'] if a.startswith('-DCMAKE_C_COMPILER=')))
output=lane.parent/'PS1B-165-pair'/f'cd-source-{args.attempt}'
output.mkdir()
tape=output/'authored-zero-words.bin'
tape.write_bytes(b'PSX-CD-RNG1'.ljust(16,b'\0')+struct.pack('<I',64)+bytes(256))
sys.path.insert(0,str(core/'tools/public_studio'))
from backend import Runner
runner=Runner(log=lambda line:print(line,flush=True))
environment={key:value for key,value in os.environ.items() if not key.startswith('PSX_')}
environment['PATH']=str(compiler.parent)+os.pathsep+environment.get('PATH','')
names={'receipt':'source_clock_receipt','clock_rows':'source_clock_rows','drive_rows':'drive_profile_rows','cdda':'source_cdda','snapshot':'source_snapshot'}
selected=args.include.split(',')
if not selected or set(selected)-set(names):raise SystemExit('Unknown/empty fixture selection')
executed=0
for optimization in ('-O0','-O2'):
    for name in selected:
        binary=output/(name+optimization[1:]+'.exe')
        runner.run([compiler,optimization,'-std=c11','-I',core/'runtime/include','-I',core/'runtime/src',
                    core/f'runtime/tests/test_cdrom_{names[name]}.c',core/'runtime/src/psx_sha256.c','-o',binary],output,env=environment)
        if name=='cdda':
            runner.run([sys.executable,'-I','-X','utf8','-B',core/'runtime/tests/test_cdrom_source_cdda_contracts.py',binary],output,env=environment)
        else:
            fixture_tape=tape if name=='receipt' else core/'runtime/tests/cd_source_cold_random_256.psxrng'
            runner.run([binary,fixture_tape],output,env=environment)
        executed+=1
assert executed==len(selected)*2
print(f'CD_SOURCE_PROFILE_OPTIMIZATIONS_PASS=2 FIXTURE_EXECUTIONS={executed}',flush=True)
