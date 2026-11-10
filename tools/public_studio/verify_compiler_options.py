"""Exercise selected core's real CMake IPO helper and final generated-code flags."""
from pathlib import Path
import ast
import argparse
import json
import os
import sys
import compiler_options
from backend import Runner

if '--no-op' in sys.argv:
    print('noop: four small generated C/C++ compiler-choice fixtures; no game')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core',type=Path,default=root/'pin-cache/c')
parser.add_argument('--output',type=Path,default=root/'reports/compiler-option-checks-1')
parser.add_argument('--source-cli',action='store_true')
args=parser.parse_args()
output=args.output;output.mkdir()
core=args.core
namespace={}
if not args.source_cli:
    function=next(n for n in ast.parse((core/'psxrecomp_cli.py').read_text(encoding='utf-8')).body if isinstance(n,ast.FunctionDef) and n.name=='product_cmake_flags')
    exec(compile(ast.Module(body=[function],type_ignores=[]),str(core/'psxrecomp_cli.py'),'exec'),namespace)
runner=Runner(lambda line:print(line,flush=True));results=[]
bin=root/'inputs/toolchain/bin';environment=os.environ.copy();environment['PATH']=str(bin)+os.pathsep+environment.get('PATH','')
for mode in ('fast','optimized'):
    for lto in (False,True):
        case=output/f'{mode}-{lto}';case.mkdir();source=case/'source';source.mkdir()
        (source/'generated.c').write_text('int generated(void) { return 7; }\n',encoding='utf-8')
        (source/'native.cpp').write_text('extern "C" int generated(void); int main(){return generated()==7?0:1;}\n',encoding='utf-8')
        cmake='cmake_minimum_required(VERSION 3.24)\nproject(compiler_choices C CXX)\ninclude("'+(core/'cmake/psx_runtime_ipo.cmake').as_posix()+'")\nadd_executable(psx-runtime generated.c native.cpp)\npsxrecomp_apply_runtime_ipo(psx-runtime)\n'
        (source/'CMakeLists.txt').write_text(cmake,encoding='utf-8')
        choices=compiler_options.arguments(mode,lto,source=args.source_cli)
        if args.source_cli:
            runner.run([sys.executable,core/'psxrecomp_cli.py','rebuild',*choices,'--help'],case,env=environment)
        extras=[a.removeprefix('--cmake-extra=') for a in choices if a.startswith('--cmake-extra=')]
        product_flags=[] if args.source_cli else namespace['product_cmake_flags'](lto)
        argv=[bin/'cmake.exe','-S',source,'-B',case/'build','-G','Ninja','-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_C_COMPILER={(bin/"clang.exe").as_posix()}',f'-DCMAKE_CXX_COMPILER={(bin/"clang++.exe").as_posix()}',*product_flags,*extras]
        runner.run(argv,case,env=environment);runner.run([bin/'cmake.exe','--build',case/'build','-j','1'],case,env=environment)
        result=compiler_options.verify(case/'build',mode,lto)
        runner.run([case/'build/psx-runtime.exe'],case,env=environment)
        results.append(result)
(output/'result.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
print('COMPILER_CHOICE_CASES_PASS='+str(len(results)),flush=True)
