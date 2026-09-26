"""Run clean authored L1 RAM against a complete owned runtime build."""
from pathlib import Path
import argparse,json,os,struct,subprocess,hashlib
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--case');p.add_argument('--precise',action='store_true');p.add_argument('--native',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];b=a.build.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
s=(b/'build.ninja').read_text();lines=s.splitlines();line=next(l for l in lines if ': CXX_EXECUTABLE_LINKER__psx-runtime_' in l);objs=line.split('Release ',1)[1].split(' | ',1)[0].split();libs=next(l.split(' = ',1)[1] for l in lines[lines.index(line):] if l.startswith('  LINK_LIBRARIES =')).split();libs=[l for l in libs if l not in ['-mwindows','-lSDL2main']]
obj=out/'driver.o';subprocess.run(['gcc','-O2',*(['-DL1_NATIVE'] if a.native else []),'-DPSX_ENABLE_BLOCK_CYCLES=1','-DPSX_NO_DEBUG_TOOLS','-I'+str(root/'runtime/include'),'-c',str(root/'runtime/tests/l1_runtime_driver.c'),'-o',str(obj)],check=True)
def link(extra):
 linked=[x for x in objs if not (a.native and 'game_dispatch_compat.c.obj' in x)]
 args=['-static','-static-libgcc','-static-libstdc++',str(obj)]+extra+linked+libs+['-o',str(out/'test.exe')];(out/'link.rsp').write_text('\n'.join('"'+x.replace('\\','/')+'"' for x in args));subprocess.run(['g++','@'+str(out/'link.rsp')],cwd=b,check=True)
if not a.native:link([])
j=json.loads((root/'runtime/tests/load_delay_l1_clean.json').read_text());env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')};env.update(PSX_TIMER1_MODEL='octoshock-2.2.2',PSX_TIMER2_MODEL='octoshock-2.2.2',PSX_GPU_DMA_MODEL='octoshock-2.2.2-bounded-linked-list',PSX_INPUT_ROUTE_FIELD_MODEL='octoshock-2.2.2-ntsc-raster',PSX_INPUT_ROUTE_FILE='authored-fixture-no-playback',PSX_CRITICAL_SECTION_MODEL='exception',PSX_ICACHE='1',PSX_BIOS_HLE='0',PSX_LEGACY_CARD_REPAIR='0')
identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'driver_sha256':hashlib.sha256((root/'runtime/tests/l1_runtime_driver.c').read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256((root/'runtime/tests/load_delay_l1_clean.json').read_bytes()).hexdigest(),'build_ninja_sha256':hashlib.sha256((b/'build.ninja').read_bytes()).hexdigest(),'mode':'native-with-precise' if a.native and a.precise else 'native' if a.native else 'precise' if a.precise else 'dirty','environment':{k:v for k,v in env.items() if k.startswith('PSX_')},'objects':[{'path':x,'sha256':hashlib.sha256((b/x).read_bytes()).hexdigest()} for x in objs]}
(out/'identity.json').write_text(json.dumps(identity,indent=2))
rows=[]
for c in j['cases']:
 if a.case and c['case']!=a.case:continue
 ram=bytearray(0x200000)
 for addr,words in [(0x80,[0x08000080,0]),(0x200,j['handler']),(0x1000,c['code']),(0x40000,c['data'])]:struct.pack_into('<'+'I'*len(words),ram,addr,*words)
 if a.native:
  gen=out/c['case'];gen.mkdir(exist_ok=True)
  header=bytearray(2048);header[:8]=b'PS-X EXE'
  for offset,value in [(0x10,0x80001000),(0x18,0x80000000),(0x1c,0x40040)]:struct.pack_into('<I',header,offset,value)
  (gen/'l1.psx').write_bytes(header+ram[:0x40040]);(gen/'seeds.txt').write_text('0x80001000\n0x80000080\n0x80000200\n')
  subprocess.run([str(a.native.resolve()),str(gen/'l1.psx'),'--seeds',str(gen/'seeds.txt'),'--out-dir',str(gen)],check=True,capture_output=True)
  extra=[]
  for src in list(gen.glob('*_full*.c'))+list(gen.glob('*_dispatch.c')):
   target=src.with_suffix('.o');subprocess.run(['gcc','-O2','-DPSX_ENABLE_BLOCK_CYCLES=1','-DPSX_NO_DEBUG_TOOLS','-I'+str(root/'runtime/include'),'-c',str(src),'-o',str(target)],check=True);extra.append(str(target))
  link(extra)
 (out/'ram.bin').write_bytes(ram);q=subprocess.run([str(out/'test.exe'),str(out/'ram.bin')]+(['precise'] if a.precise else []),cwd=out,env=env,capture_output=True,text=True,timeout=20);(out/(c['case']+'.log')).write_text(q.stdout+q.stderr)
 try:got=[int(x,16) for x in q.stdout.strip().splitlines()[-1].split()]
 except (ValueError,IndexError):got=[]
 ok=q.returncode==0 and got==c['expected'];rows.append(dict(case=c['case'],exit=q.returncode,match=ok,actual=got,expected=c['expected']));print(c['case'],q.returncode,ok,flush=True)
(out/'receipt.json').write_text(json.dumps(rows,indent=2))
raise SystemExit(0 if all(r['match'] for r in rows) else 1)
