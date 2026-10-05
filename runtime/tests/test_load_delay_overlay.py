"""Real loader + generated DLL carry test; timing callbacks use authored seams."""
from pathlib import Path
import argparse,os,platform,struct,subprocess,tempfile,zlib
import test_overlay_pair_dedup_runtime as base
ap=argparse.ArgumentParser();ap.add_argument('--recompiler',type=Path,required=True);args=ap.parse_args()
here=Path(__file__).resolve().parent;runtime=here.parent
cc=os.environ.get("CC", "gcc")
words=[0x03e00008,0x8e2b0000]+[0]*6+[0x01606021,0x01606825,0x03e00008,0]
code=struct.pack('<'+'I'*len(words),*words)
with tempfile.TemporaryDirectory() as td:
 root=Path(td);header=bytearray(2048);header[:8]=b'PS-X EXE'
 for off,val in [(0x10,0x80010000),(0x18,0x80010000),(0x1c,len(code))]:struct.pack_into('<I',header,off,val)
 (root/'load.psx').write_bytes(header+code);(root/'seeds.txt').write_text('0x80010000\n0x80010020\n')
 out=root/'generated';out.mkdir()
 base.run([str(args.recompiler.resolve()),str(root/'load.psx'),'--project-root',str(runtime.parent),'--seeds',str(root/'seeds.txt'),'--out-dir',str(out)])
 shim=root/'shim.c';shim.write_text('#include "overlay_dispatch_preamble.c.inc"\nPSX_OVERLAY_EXPORT uint64_t overlay_pair_id(void){return UINT64_C(0x1020304050607080); }\n')
 harness=root/'harness.c'
 harness.write_text('#define main pair_original_main\n#define psx_cyc_load_word pair_unused_word\n#define psx_cyc_load_word_slow pair_unused_word_slow\n#include "PAIR_HARNESS"\n#undef main\n#undef psx_cyc_load_word\n#undef psx_cyc_load_word_slow\n#include <assert.h>\nuint32_t psx_cyc_load_word(CPUState*c,uint32_t addr,uint32_t rt,uint32_t mask){\n (void)c;(void)rt;(void)mask;assert((addr&0x1fffffff)==0x40000u);return 0xbbbb0002u;\n}\nuint32_t psx_cyc_load_word_slow(CPUState*c,uint32_t addr,uint32_t rt,uint32_t mask){return psx_cyc_load_word(c,addr,rt,mask);}\nint main(int argc,char**argv){\n assert(argc==3);FILE*f=fopen(argv[2],"rb");assert(f);assert(fread(s_ram+0x10000,1,48,f)==48);fclose(f);\n overlay_loader_init(argv[1],"PAIR-TEST",0);\n fprintf(stderr,"overlay registered=%d message=%s\\n",overlay_loader_registered_count(),overlay_loader_last_msg());\n assert(overlay_loader_registered_count()==2);\n CPUState cpu={0};cpu.gpr[11]=0xaaaa0001u;cpu.gpr[17]=0xa0040000u;cpu.gpr[31]=0x80010020u;\n assert(overlay_loader_dispatch(&cpu,0x80010000u));\n assert(cpu.gpr[11]==0xaaaa0001u&&cpu.load_value_rt==11&&cpu.load_value==0xbbbb0002u);\n cpu.gpr[31]=0;\n assert(overlay_loader_dispatch(&cpu,0x80010020u));\n assert(cpu.gpr[12]==0xaaaa0001u&&cpu.gpr[13]==0xbbbb0002u&&!cpu.load_value_rt);\n /* Host-armed pending state must be observed by generated DLL entry. */\n cpu.gpr[11]=0x11112222u;psx_load_value_arm(&cpu,11,0x33334444u);\n assert(overlay_loader_dispatch(&cpu,0x80010020u));\n assert(cpu.gpr[12]==0x11112222u&&cpu.gpr[13]==0x33334444u);\n puts("PASS: generated DLL load-slot -> loader return -> generated DLL consumer, and host -> DLL pending state");\n return 0;\n}\n'.replace('PAIR_HARNESS',(here/'overlay_pair_dedup_harness.c').as_posix()))
 for opt in ['-O0','-O2']:
  dll=root/('fixture'+opt+('.dll' if os.name=='nt' else '.so'))
  cmd=[cc,'-shared',opt,'-DPSX_OVERLAY_DLL_BUILD','-DPSX_NO_DEBUG_TOOLS','-DPSX_ENABLE_BLOCK_CYCLES=1','-DPSX_OVERLAY_FLAVOR=0','-I'+str(runtime/'include'),str(shim)]+[str(p) for p in out.glob('*_full*.c')]+['-lm','-o',str(dll)]
  if os.name!='nt':cmd.append('-fPIC')
  else:cmd.append('-Wl,--export-all-symbols')
  base.run(cmd)
  cache=root/('cache'+opt);manifest=f'P {base.PAIR:016X}\n'
  for pc,start,length in [(0x80010000,0,8),(0x80010020,32,16)]:
   manifest+=f'F {pc:08X} {zlib.crc32(code[start:start+length])&0xffffffff:08X}\nR {pc:08X} {length:08X}\n'
  base.publish(cache,'gcc','00010000_11111111'+dll.suffix,dll,manifest)
  exe=root/('harness'+opt+('.exe' if os.name=='nt' else ''))
  cmd=[cc,opt,'-std=c11','-DPSX_NO_DEBUG_TOOLS','-DPSX_OVERLAY_DLL_BUILD','-DPSX_OVERLAY_TEST_CANDIDATE_CAP=4','-I'+str(runtime/'include')]+[str(runtime/'src'/p) for p in ['overlay_loader.c','overlay_path_canon.c','overlay_posix.c','crc32.c']]+[str(harness),'-o',str(exe)]
  if os.name!='nt':cmd+=['-D_GNU_SOURCE','-ldl','-pthread']
  base.run(cmd);(root/'code.bin').write_bytes(code);base.run([str(exe),str(cache),str(root/'code.bin')]);print(opt,'real overlay transport PASS',flush=True)
