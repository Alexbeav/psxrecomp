"""Run authored L1 instructions through the real decoder; unexpected seams abort."""
import argparse
from pathlib import Path
import tempfile
import struct
import subprocess
from source_fixture_link import build_and_run
import source_fixture_link

def named_stubs(path, symbols):
    path.write_text('#include <stdio.h>\n#include <stdlib.h>\n' + '\n'.join(
        f'void {s}(void) {{ fprintf(stderr,"unexpected seam: {s}\\n"); exit(99); }}'
        for s in sorted(symbols)))
source_fixture_link.write_stubs = named_stubs

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--recompiler', type=Path)
    ap.add_argument('--bios-emitter', type=Path)
    args = ap.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as tmp:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(tmp),
                          [str(here / 'load_delay_decoder.c')], 'test_load_delay_l1.c')
        if args.recompiler:
            root = Path(tmp)
            cases = [
                ([0x8e2b0000,0x01606021,0x01606825],0xaaaa0001,0xbbbb0002),
                ([0x8e2b0000,0x8e2b0004,0x01606021,0x01606825],0xaaaa0001,0xcccc0003),
                ([0x8a2b0005,0x9a2b0002,0x01606021,0x01606825],0x11223344,0x0003bbbb),
                ([0x8e2b0000,0x240b0077,0x01606021,0x01606825],0x77,0x77),
                ([0x10000002,0x8e2b0000,0,0x01606021,0x01606825],0xaaaa0001,0xbbbb0002),
                ([0x04110002,0x8e2b0000,0,0x01606021,0x01606825],0xaaaa0001,0xbbbb0002),
                ([0x07f00002,0x8e2b0000,0x240c0077,0x01606825],0x77,0xbbbb0002),
            ]
            image = bytearray(0x100*len(cases))
            for i,(words,_,_) in enumerate(cases):
                words=words+[0x03c00008 if i>=5 else 0x03e00008,0]
                struct.pack_into('<'+'I'*len(words),image,i*0x100,*words)
            header=bytearray(2048);header[:8]=b'PS-X EXE'
            for offset,value in [(0x10,0x80010000),(0x18,0x80010000),(0x1c,len(image))]:
                struct.pack_into('<I',header,offset,value)
            exe=root/'l1.psx';exe.write_bytes(header+image)
            seeds=root/'seeds.txt';seeds.write_text(''.join(f'0x{0x80010000+i*0x100:08x}\n' for i in range(len(cases))))
            out=root/'generated';out.mkdir()
            subprocess.run([str(args.recompiler.resolve()),str(exe),'--project-root',str(here.parents[1]),'--seeds',str(seeds),'--out-dir',str(out)],
                           check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            driver=root/'native.c'
            lines=['#define main decoder_only_main',f'#include "{(here / "test_load_delay_l1.c").as_posix()}"',
                   '#undef main', 'static uint8_t ram[2*1024*1024];',
                   'uint8_t *memory_get_ram_ptr(void) {return ram;}',
                   'void debug_server_log_call_entry(uint32_t p) {(void)p;}',
                   'void debug_server_cyc_observe(uint32_t p) {(void)p;}',
                   'void psx_check_interrupts(CPUState *c) {(void)c;}',
                   'void psx_check_interrupts_at(CPUState *c,uint32_t p) {(void)c;(void)p;}',
                   'int main(void) { decoder_only_main(); CPUState c; uint32_t next;']
            for i,(words,x,y) in enumerate(cases):
                words=words+[0x03c00008 if i>=5 else 0x03e00008,0]
                address=0x80010000+i*0x100
                lines += [f'extern void func_{address:08X}(CPUState *);',
                          f'const uint32_t program_{i}[]={{'+','.join(hex(w)+'u' for w in words)+'};',
                          f'memcpy(ram+{address&0x1fffffff},program_{i},sizeof program_{i});',
                          'c=fresh();c.gpr[31]=0;',f'c.pc=0x{address:08x}u;']
                if i==2:lines += ['c.gpr[11]=0x11223344u;']
                lines += ['for(unsigned n=0;c.pc && n<30;n++) { uint32_t w;memcpy(&w,ram+(c.pc&0x1fffffff),4);',
                          'if(!l1_step(&c,c.pc,w,&next))c.pc=next;}', 'assert(c.pc==0);',
                          f'equal(c.gpr[12],0x{x:08x}u);equal(c.gpr[13],0x{y:08x}u);',
                          'c=fresh();c.gpr[31]=0;',f'c.pc=0x{address:08x}u;']
                if i==2:lines += ['c.gpr[11]=0x11223344u;']
                lines += [f'func_{address:08X}(&c);',f'equal(c.gpr[12],0x{x:08x}u);equal(c.gpr[13],0x{y:08x}u);']
            lines+=['return 0;}'];driver.write_text('\n'.join(lines))
            modules=[str(here/'load_delay_decoder.c')]+[str(p) for p in out.glob('*_full*.c')]
            for opt in ('-O0','-O2'):
                build_and_run(args.cc,root,here.parent,opt,root,modules,str(driver))
            if args.bios_emitter:
                for mode in ('load-value','load-value-link'):
                    bios=root/mode;bios.mkdir()
                    subprocess.run([str(args.bios_emitter.resolve()),str(bios),mode],check=True,
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                    extra=['extern void Test_func_1FC00000(CPUState *);',
                        'c=fresh();c.pc=0;c.gpr[31]=0;Test_func_1FC00000(&c);',
                        'equal(c.gpr[12],0xaaaa0001u);equal(c.gpr[13],0xbbbb0002u);']
                    if mode.endswith('link'):extra+=['equal(c.gpr[31],0x508u);']
                    driver.write_text('\n'.join(lines[:-1]+extra+lines[-1:]))
                    for opt in ('-O0','-O2'):
                        build_and_run(args.cc,root,here.parent,opt,root,
                                      modules+[str(bios/'Test_full.c')],str(driver))
    print('PASS: 32 decoder/save checks at O0/O2' +
          ('; seven actual CFG-native comparisons at O0/O2' if args.recompiler else '') +
          ('; two full-function native comparisons at O0/O2' if args.bios_emitter else ''))
