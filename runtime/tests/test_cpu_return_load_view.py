"""Read actual observer TSV; observing must leave all execution state untouched."""
import argparse,csv,os,subprocess,tempfile
from pathlib import Path
PROGRAM=r"""
#include <assert.h>
#include <string.h>
#include "cpu_state.h"
uint32_t g_psx_icache_tv[1024];int g_psx_icache_active;
int g_precise_mode,g_dirty_interp_active;
uint32_t i_stat,i_mask,g_slice_probe_pc,g_slice_probe_deadline,g_slice_probe_bound;
uint64_t g_slice_probe_cycle,g_slice_irq_taken;
uint32_t dma_cpu_read_penalty(void) {assert(0);return 0;}
#include "source_cpu_boundary_probe.h"
static void snapshot(CPUState *c,unsigned frame) {
 CPUState before=*c;
 source_cpu_return_probe(c,0x80001000u+frame*4u,frame*7u,frame);
 assert(!memcmp(c,&before,sizeof before));
}
int main(void) {
 CPUState c={0};c.gpr[11]=0xaaaa0001u;
 psx_load_value_arm(&c,11,0xbbbb0002u);snapshot(&c,1);
 psx_load_value_begin(&c);c.gpr[12]=c.gpr[11];snapshot(&c,2);
 psx_load_value_begin(&c);snapshot(&c,3);
 c.gpr[11]=0xaaaa0001u;psx_load_value_arm(&c,11,0xbbbb0002u);
 psx_load_value_begin(&c);psx_load_value_arm(&c,11,0xcccc0003u);snapshot(&c,4);
 psx_load_value_begin(&c);snapshot(&c,5);
 psx_load_value_cancel(&c,11);c.gpr[11]=0x77;snapshot(&c,6);
 return 0;
}
"""
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--cc',default='gcc');args=ap.parse_args()
 root=Path(__file__).resolve().parents[1]
 with tempfile.TemporaryDirectory() as tmp:
  p=Path(tmp);(p/'test.c').write_text(PROGRAM)
  for opt in ['-O0','-O2']:
   out=p/opt;out.mkdir()
   subprocess.run([args.cc,'-std=c11',opt,'-I',str(root/'include'),str(p/'test.c'),'-o',str(out/'test.exe')],check=True)
   env={k:v for k,v in os.environ.items() if not k.upper().startswith('PSX_')}
   env.update(PSX_SOURCE_CPU_RETURN_PROBE='1',PSX_INPUT_ROUTE_CAPTURE_DIR=str(out))
   subprocess.run([str(out/'test.exe')],env=env,check=True)
   with (out/'cpu-return.tsv').open() as f:rows=list(csv.DictReader(f,delimiter='\t'))
   got=[int(r['r11'],16) for r in rows]
   expected=[0xaaaa0001,0xbbbb0002,0xbbbb0002,0xaaaa0001,0xcccc0003,0x77]
   assert got==expected,(opt,[hex(v) for v in got],[hex(v) for v in expected])
   assert all(int(r['r0'],16)==0 for r in rows)
   assert all(int(r['r12'],16)==0xaaaa0001 for r in rows[1:])
   print('PASS:',opt,'six passive observer rows, CPUState unchanged')
