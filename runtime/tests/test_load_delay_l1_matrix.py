"""Replay all clean L1 programs; IRQs are injected at the observed EPC.
This isolates CPU value semantics. It does not qualify timer IRQ recognition.
"""
from pathlib import Path
import json, tempfile, argparse, struct, subprocess, os
ap=argparse.ArgumentParser();ap.add_argument("--precise",action="store_true");ap.add_argument("--native",type=Path);ap.add_argument("--case");ap.add_argument("--keep",type=Path);args=ap.parse_args()
import source_fixture_link as link
here=Path(__file__).resolve().parent
fixture=json.loads((here/'load_delay_l1_clean.json').read_text())
def named(path,symbols):
 path.write_text('#include <stdio.h>\n#include <stdlib.h>\n'+'\n'.join(f'void {s}(void){{fprintf(stderr,"unexpected seam {s}\\n");exit(99);}}' for s in sorted(symbols)))
link.write_stubs=named
def run_cases(selected,tmp,native=False):
 arrays=['static const uint32_t handler[]={'+','.join(hex(x)+'u' for x in fixture['handler'])+'};']
 for i,c in enumerate(selected):
  for key in ['code','data','expected']:
   arrays.append(f'static const uint32_t {key}_{i}[]={{'+','.join(hex(x)+'u' for x in c[key])+'};')
 arrays+=['typedef struct {const char *name;const uint32_t *code,*data,*expected;unsigned size;} Case;','static const Case cases[]={']
 for i,c in enumerate(selected):arrays.append('{"'+c['case']+f'",code_{i},data_{i},expected_{i},sizeof code_{i}'+'},')
 arrays+=['};']
 (tmp/'matrix.h').write_text('\n'.join(arrays))
 source=(here/'test_load_delay_l1_matrix.c.in').read_text(encoding='utf-8-sig').replace('@MATRIX@',(tmp/'matrix.h').as_posix())
 modules=[str(here/'load_delay_decoder.c'),'gte.cpp']
 if native:
  c=selected[0];ram=bytearray(0x40040)
  for addr,words in [(0x80,[0x08000080,0]),(0x200,fixture['handler']),(0x1000,c['code']),(0x40000,c['data'])]:
   struct.pack_into('<'+'I'*len(words),ram,addr,*words)
  header=bytearray(2048);header[:8]=b'PS-X EXE'
  for addr,val in [(0x10,0x80001000),(0x18,0x80000000),(0x1c,len(ram))]:struct.pack_into('<I',header,addr,val)
  (tmp/'l1.psx').write_bytes(header+ram)
  (tmp/'seeds.txt').write_text('0x80001000\n0x80000080\n0x80000200\n')
  out=tmp/'generated';out.mkdir()
  subprocess.run([str(args.native.resolve()),str(tmp/'l1.psx'),'--seeds',str(tmp/'seeds.txt'),'--out-dir',str(out)],check=True,capture_output=True)
  modules += [str(p) for p in out.glob('*_full*.c')]+[str(p) for p in out.glob('*_dispatch.c')]
  source=source.replace('int main(void){','int g_psx_cps_mode;\nint psx_vsync_query_hle_try(CPUState*c,uint32_t p){(void)c;(void)p;return 0;}\nint dirty_ram_text_native_ok_ranges_from(const uint32_t *r,uint32_t n,uint32_t p){(void)r;(void)n;(void)p;return 1;}\nint psx_dispatch_game_compiled(CPUState*,uint32_t);\nvoid debug_server_log_call_entry(uint32_t p){(void)p;}\nvoid debug_server_cyc_observe(uint32_t p){(void)p;}\nvoid psx_check_interrupts(CPUState*c){(void)c;}\nvoid psx_check_interrupts_at(CPUState*c,uint32_t p){(void)c;(void)p;}\nvoid psx_check_interrupts_dispatch_entry(CPUState*c,uint32_t p){(void)c;(void)p;}\nint main(void){')
  source=source.replace('uint32_t next=0;if(!l1_step(&c,c.pc,psx_read_word(c.pc),&next))c.pc=next;',
   'if(!psx_dispatch_game_compiled(&c,c.pc)){fprintf(stderr,"native dispatch miss %08x\\n",c.pc);return 1;}')
 if args.precise:
  source=source.replace(' l1_precise_control();',' g_psx_precise_slice=1;')
  source=source.replace('int main(void){',(here/'load_delay_precise_seams.c.in').read_text()+'\nint main(void){')
  source=source.replace('uint32_t next=0;if(!l1_step(&c,c.pc,psx_read_word(c.pc),&next))c.pc=next;',
   'assert(psx_slice_block_impl(&c,c.pc,1,1));')
 (tmp/'matrix.c').write_text(source)
 for opt in ['-O0','-O2']:
  link.build_and_run(os.environ.get('CC', 'gcc'),tmp,here.parent,opt,tmp,modules,'matrix.c')
with tempfile.TemporaryDirectory() as td:
 root=args.keep.resolve() if args.keep else Path(td);root.mkdir(exist_ok=True,parents=True)
 if args.native:
  selected=[c for c in fixture['cases'] if not c['case'].startswith('g-') and (not args.case or c['case']==args.case)]
  for c in selected:
   tmp=root/c['case'];tmp.mkdir();run_cases([c],tmp,True);print(c['case'],'native O0/O2 PASS',flush=True)
 else:run_cases([c for c in fixture['cases'] if not args.precise or not c['case'].startswith('g-')],root)
print('L1 matrix PASS; timer IRQ transport and full overlay matrix remain separate gates')

