"""Long-route coverage and fail-closed bounds using the actual RAM observer."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

def fnv(data):
    h = 14695981039346656037
    for byte in data:
        h = ((h ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{h:016X}'

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='gcc')
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    ram = bytearray(2097152)
    ram[0], ram[-1] = 0x34, 0xab
    zero = fnv(bytes(4096))
    expected = [fnv(ram[:4096])] + [zero]*510 + [fnv(ram[-4096:])]
    cases = [('legacy',None,20000,None,0),
             ('legacy_limit',None,20001,None,4),
             ('pepsiman','71806',71806,'71806',0),
             ('re1','227202',227202,'227202',0),
             ('bound','71806',71807,None,4),
             ('snapshot_bound','71806',1,'71807',4),
             ('duplicate','71806',1,'71806,71806',4),
             ('zero','0',1,None,4), ('negative','-1',1,None,4),
             ('junk','71806x',1,None,4), ('overflow','999999999999999999999',1,None,4),
             ('space',' 71806',1,None,4), ('over_cap','1000001',1,None,4)]
    with tempfile.TemporaryDirectory() as temporary:
        root=Path(temporary)
        for opt in ['-O0','-O2']:
            exe=root/('probe'+opt+('.exe' if os.name=='nt' else ''))
            subprocess.run([a.cc,'-std=c11',opt,'-Wall','-Wextra','-Werror',
                            '-I'+str(here.parent/'include'),str(here/'test_source_ram_page_probe.c'),
                            '-o',str(exe)],check=True)
            for name,limit,frame,snapshot,code in cases:
                out=root/(name+opt)
                out.mkdir()
                env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
                env.update(PSX_SOURCE_RAM_PAGE_PROBE='1',PSX_INPUT_ROUTE_CAPTURE_DIR=str(out))
                if limit is not None: env['PSX_SOURCE_RAM_MAX_FRAMES']=limit
                if snapshot: env['PSX_SOURCE_RAM_SNAPSHOT_FRAMES']=snapshot
                result=subprocess.run([str(exe),str(frame)],env=env,capture_output=True,timeout=20)
                assert result.returncode==code,(opt,name,result.returncode,code,result.stderr)
                if code==0:
                    rows=(out/'ram-pages.tsv').read_text().splitlines()
                    assert len(rows)==3,(name,len(rows))
                    row=rows[2].split('\t')
                    assert row[:2]==[str(frame),'40500000000'] and row[2:]==expected,name
                    if snapshot:
                        assert (out/f'ram-frame-{frame:06d}.bin').read_bytes()==ram,name
                else:
                    assert not (out/'ram-pages.tsv').exists(),name
    print(f'PASS: {len(cases)*2} production RAM observer cases; all 512 pages, clocks and late raw snapshots')

if __name__=='__main__': main()
