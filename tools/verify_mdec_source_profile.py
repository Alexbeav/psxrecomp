"""Verify the maintained optional MDEC profile on authored inputs."""
import argparse
import json
import os
from pathlib import Path
import sys
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--attempt', type=int, required=True)
parser.add_argument('--include', default='receipt,fifo,pipeline')
parser.add_argument('--no-op', action='store_true')
args = parser.parse_args()
if args.no_op:
    print('noop: one serial authored MDEC profile driver at O0/O2')
    raise SystemExit(0)
selected = args.include.split(',')
if not selected or set(selected) - {'receipt', 'fifo', 'pipeline', 'dma', 'clean', 'snapshot', 'bus-receipt', 'bus-clock', 'bus-command', 'bus-spec', 'bus-regs', 'bus-cd', 'bus-otc', 'bus-ll', 'bus-upload', 'bus-compile', 'bus-raster', 'bus-service', 'bus-runtime', 'bus-state', 'bus-codegen-preview', 'bus-codegen', 'bus-frozen', 'bus-prefix', 'bus-overlay', 'bus-cd-clock'}:
    raise SystemExit('unknown or empty fixture selection')
core = Path(__file__).resolve().parents[1]
lane = core.parent
config = json.loads((lane / 'reports/consumer-790-config.json').read_text(encoding='utf-8'))
cc = Path(next(a.split('=', 1)[1] for a in config['configure_args'] if a.startswith('-DCMAKE_C_COMPILER=')))
out = lane.parent / 'PS1B-165-pair' / f'mdec-source-{args.attempt}'
out.mkdir()
sys.path.insert(0, str(core / 'tools/public_studio'))
from backend import Runner
runner = Runner(log=lambda line: print(line, flush=True))
env = os.environ.copy()
env['PATH'] = str(cc.parent) + os.pathsep + env.get('PATH', '')
env['PSX_MDEC_SOURCE_MODEL'] = 'octoshock-2.3'
executed = 0
probe_header = None
for opt in ('-O0', '-O2'):
    for name in selected:
        if name == 'bus-overlay':
            for feature in (0,1):
                binary=out/('overlay-'+str(feature)+'-'+opt[1:]+'.exe')
                defs=['-DPSX_SOURCE_BUS_BOUNDARY=1','-DPSX_OVERLAY_FLAVOR=4'] if feature else []
                runner.run([cc,opt,'-std=c11','-I',core/'runtime/include',*defs,
                            core/'runtime/tests/test_source_bus_overlay.c','-o',binary],out,env=env)
                runner.run([binary],out,env=env)
                executed+=1
            continue
        if name in ('bus-codegen-preview','bus-codegen'):
            sys.path.insert(0,str(core/'recompiler/tests'))
            from source_bus_codegen_probe import generate_probe
            if probe_header is None:
                probe_header=generate_probe(lane.parent/'PS1B-165-pair/e/psxrecomp-game.exe',out)
            header=probe_header
            if name == 'bus-codegen':
                sys.path.insert(0,str(core/'runtime/tests'))
                from test_source_bus_fixtures import run_case
                print(run_case(str(cc),core/'runtime/tests',opt,out,'runtime',
                      extra_defines=('PSX_TEST_GENERATED_SOURCE="'+header.as_posix()+'"',
                                     'PSX_TEST_GENERATED_ENTRY=func_80010000')),flush=True)
            else:
                print("SOURCE_BUS_CODEGEN_PREVIEW=1",flush=True)
                raise SystemExit(0)
            executed+=1
            continue
        if name in ('bus-runtime','bus-service','bus-raster','bus-frozen','bus-prefix','bus-cd-clock'):
            sys.path.insert(0,str(core/'runtime/tests'))
            from test_source_bus_fixtures import run_case
            print(run_case(str(cc),core/'runtime/tests',opt,out,name[4:]),flush=True)
            executed+=1
            continue
        if name == 'bus-compile':
            for module in ('dma.c','source_gpu_runtime.c','gpu.c','gpu_sw_renderer.c','gpu_render.c','psx_cycles.c','interrupts.c','memory.c'):
                runner.run([cc,opt,'-std=c11','-fsyntax-only','-DPSX_SDL3=1','-DPSX_SOURCE_BUS_BOUNDARY=1',
                            '-DPSX_ENABLE_BLOCK_CYCLES=1','-I',core/'runtime/include','-I',core/'runtime/src',
                            '-I',lane/'inputs/SDL3/include','-I',lane/'inputs/SDL3/include/SDL3',core/'runtime/src'/module],out,env=env)
            executed+=1
            continue
        if name in ('bus-state','bus-clock','bus-command','bus-spec','bus-regs','bus-cd','bus-otc','bus-ll','bus-upload'):
            source={'bus-state':'test_dma_source_state.c','bus-clock':'test_source_gpu_service_clock.c','bus-command':'test_source_gpu_command_projection.c',
                    'bus-spec':'test_dma_source_spec2.c','bus-regs':'test_dma_source_register_edges.c',
                    'bus-cd':'test_dma_cd_source.c','bus-otc':'test_dma_otc_timing.c',
                    'bus-ll':'test_dma_gpu_linked_list_source.c','bus-upload':'test_dma_gpu_upload_source.c'}[name]
            binary=out/(name+'-'+opt[1:]+'.exe')
            runner.run([cc,opt,'-std=c11','-I',core/'runtime/include','-I',core/'runtime/src',core/'runtime/tests'/source,'-o',binary],out,env=env)
            runner.run([binary, "16", "1"] if name == "bus-clock" else [binary],out,env=env)
            executed+=1
            continue
        if name == 'bus-receipt':
            sys.path.insert(0, str(core/'runtime/tests'))
            from fixture_link import build_and_run
            print(build_and_run(str(cc),core/'runtime/tests',core/'runtime',opt,out,
                                ['dma.c','dma_gpu_ll.c','gpu_timeline.c'],'source_bus_receipt.c'),flush=True)
            executed += 1
            continue
        if name == 'clean':
            binary = out / ('clean-' + opt[1:] + '.dll')
            clean_env = dict(env)
            clean_env.pop('PSX_MDEC_SOURCE_MODEL', None)
            runner.run([cc, opt, '-shared', '-std=c11', '-I', core/'runtime/include',
                        core/'runtime/tests/mdec_clean_fixture_probe.c', '-o', binary], out, env=clean_env)
            runner.run([sys.executable, '-I', '-X', 'utf8', '-B',
                        core/'runtime/tests/test_mdec_clean_fixtures.py', '--library', binary,
                        '--entry-path', 'source', '--receipt', out/('clean-'+opt[1:]+'.json')], out, env=clean_env)
            executed += 1
            continue
        binary = out / (name + '-' + opt[1:] + '.exe')
        source = {'receipt': 'mdec_source_receipt.c',
                  'fifo': 'test_mdec_source_fifo_rows.c',
                  'pipeline': 'test_mdec_source_pipeline.c',
                  'dma': 'test_mdec_source_dma.c', 'snapshot': 'test_mdec_source_snapshot.c'}[name]
        command = [cc, opt, '-std=c11', '-I', core / 'runtime/include',
                   '-I', core / 'runtime/src', core / 'runtime/tests' / source]
        if name == 'receipt':
            command.append(core / 'runtime/src/mdec.c')
        runner.run(command + ['-o', binary], out, env=env)
        if name == 'dma':
            runner.run([binary], out, env=env)
        if name in ('pipeline', 'dma'):
            runner.run([sys.executable, '-I', '-X', 'utf8', '-B',
                        core / 'runtime/tests/test_mdec_source_contracts.py',
                        binary, name], out, env=env)
        else:
            runner.run([binary], out, env=env)
        if name == 'receipt':
            for model in ('', None, 'unknown-authored-model'):
                scope_env=dict(env)
                if model is None: scope_env.pop('PSX_MDEC_SOURCE_MODEL',None)
                else: scope_env['PSX_MDEC_SOURCE_MODEL']=model
                run=subprocess.run([str(binary), '--default'],env=scope_env,capture_output=True,
                                   text=True,encoding='utf-8',errors='replace',timeout=30)
                assert run.returncode == (2 if model else 0),(model,run.returncode,run.stderr)
            print('MDEC_SCOPE_CHECKS=3 PASS',flush=True)
        executed += 1
print(f'MDEC_SOURCE_FIXTURES_EXECUTED={executed}', flush=True)
