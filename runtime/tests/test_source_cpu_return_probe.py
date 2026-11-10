"""Production CPU-return capture preserves all registers beyond short TAS limits."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    p=argparse.ArgumentParser();p.add_argument('--cc',default='gcc');a=p.parse_args()
    here=Path(__file__).resolve().parent
    cases=[('legacy',None,20000,0),('legacy_bound',None,20001,4),
           ('pepsiman','75406',75406,0),('re1','227202',227202,0),
           ('overrun','75406',75407,4),('zero','0',1,4),('junk','123x',1,4),
           ('negative','-2',1,4),('space',' 12',1,4),('overflow','99999999999999999',1,4)]
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)
        for opt in ['-O0','-O2']:
            exe=root/('probe'+opt+('.exe' if os.name=='nt' else ''))
            subprocess.run([a.cc,'-std=c11',opt,'-Wall','-Wextra','-Werror','-I'+str(here.parent/'include'),
                            str(here/'test_source_cpu_return_probe.c'),'-o',str(exe)],check=True)
            for name,limit,frame,expected in cases:
                out=root/(name+opt);out.mkdir()
                env={k:v for k,v in os.environ.items() if not k.upper().startswith('PSX_')}
                env.update(PSX_SOURCE_CPU_RETURN_PROBE='1',PSX_INPUT_ROUTE_CAPTURE_DIR=str(out))
                if limit is not None:env['PSX_SOURCE_CPU_MAX_FRAMES']=limit
                result=subprocess.run([str(exe),str(frame)],env=env,capture_output=True,timeout=10)
                assert result.returncode==expected,(name,opt,result.returncode,expected,result.stderr)
                if expected==0:
                    lines=(out/'cpu-return.tsv').read_text().splitlines();assert len(lines)==2
                    fields=lines[1].split('\t')
                    assert fields==[str(frame),'80012340','40500000000','00001234','00005678','00009ABC']+[
                        f'{0x81000000+i:08X}' for i in range(32)],fields
    print(f'PASS: {len(cases)*2} production CPU return cases; all registers and64-bit clocks')


if __name__=='__main__':main()
