import pathlib,subprocess,json,os,sys,argparse,shutil
parser=argparse.ArgumentParser(description="Run hidden real-GL source-owned readback tests")
parser.add_argument('--compiler-bin',required=True);parser.add_argument('--sdl-root',required=True)
parser.add_argument('--output',required=True);parser.add_argument('--gl-source')
parser.add_argument('--expect-unbounded',action='store_true');args=parser.parse_args()
F=pathlib.Path(__file__).resolve().parents[2];D=pathlib.Path(args.output).resolve();D.mkdir(parents=True,exist_ok=False)
old=pathlib.Path(args.sdl_root).resolve();T=pathlib.Path(args.compiler_bin).resolve();env=os.environ.copy();env['PATH']=str(T)+os.pathsep+env['PATH']
if args.gl_source:shutil.copyfile(args.gl_source,D/'gpu_gl_renderer.c')
inc=['-I',str(D),'-I',str(F/'runtime/include'),'-I',str(F/'runtime/src'),'-I',str(old/'sdl3-src/include')];receipt=[]
def run(cmd):
 p=subprocess.run([str(x) for x in cmd],cwd=D,capture_output=True,text=True,errors='replace',env=env);receipt.append({'cmd':[str(x) for x in cmd],'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr});(D/'receipt.json').write_text(json.dumps(receipt,indent=2));print(p.stdout[-600:],p.stderr[-2000:]);return p.returncode
for name,src in [('probe',F/'runtime/tests/test_gl_readback_region.c'),('sw',F/'runtime/src/gpu_sw_renderer.c')]:
 if run([T/'gcc.exe','-std=c11','-O2','-flto','-DPSX_SDL3=1','-DPSX_NO_DEBUG_TOOLS=1',*inc,'-c',src,'-o',D/(name+'.o')]):sys.exit(2)
libs=['m','kernel32','user32','gdi32','winmm','imm32','ole32','oleaut32','version','uuid','advapi32','setupapi','shell32','dinput8','opengl32']
if run([T/'g++.exe','-O2','-flto',D/'probe.o',D/'sw.o',old/'sdl3-build/libSDL3.a',*['-l'+x for x in libs],'-o',D/'probe.exe']):sys.exit(2)
# Hidden SDL window, affinity and High priority set before tests.
import ctypes
k=ctypes.windll.kernel32;k.GetCurrentProcess.restype=ctypes.c_void_p;hp=k.GetCurrentProcess();k.SetProcessAffinityMask.argtypes=[ctypes.c_void_p,ctypes.c_size_t];k.SetPriorityClass.argtypes=[ctypes.c_void_p,ctypes.c_uint32];assert k.SetProcessAffinityMask(hp,0xFFFF);assert k.SetPriorityClass(hp,0x80)
for scale in [1,4]:
 code=run([D/'probe.exe',str(scale)])
 expected=1 if args.expect_unbounded else 0
 if code!=expected:sys.exit(1)
 if args.expect_unbounded and 'failures=1' not in receipt[-1]['stdout']:sys.exit(1)
