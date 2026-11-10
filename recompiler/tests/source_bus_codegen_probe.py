"""Generate a tiny authored guest function for source bus ownership checks."""
import hashlib
from pathlib import Path
import re
import struct
import subprocess


def generate_probe(recompiler, work):
    # nop; lw at,$a0; nop; jr ra; nop. The load must wait behind a live OTC.
    words=(0,0x8c810000,0,0x03e00008,0)
    image=bytearray(2048)
    image[:8]=b'PS-X EXE'
    struct.pack_into('<I',image,0x10,0x80010000)
    struct.pack_into('<I',image,0x18,0x80010000)
    struct.pack_into('<I',image,0x1c,len(words)*4)
    image.extend(struct.pack('<5I',*words))
    source=work/'source-bus-probe.psx'
    source.write_bytes(image)
    seeds=work/'source-bus-seeds.txt'
    seeds.write_text('0x80010000\n',encoding='utf-8')
    output=work/'generated-source-bus-probe'
    output.mkdir()
    # Profile address-window metadata is tracked with the framework. This
    # authored overlay does not read a retail BIOS image; do not depend on CWD.
    framework=Path(__file__).resolve().parents[2]
    command=[str(recompiler),str(source),'--seeds',str(seeds),'--out-dir',str(output),
             '--overlay','--project-root',str(framework)]
    result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
    if result.returncode:
        raise RuntimeError('source bus generation failed: '+result.stderr[-3000:])
    files=list(output.glob('*_full.c'))
    assert len(files)==1,files
    text=files[0].read_text(encoding='utf-8')
    match=re.search(r'void (func_80010000)\([^)]*\)\s*\{',text)
    assert match,'authored function entry missing'
    start=match.start();at=text.index('{',start)+1;depth=1
    while depth:
        if text[at]=='{':depth+=1
        elif text[at]=='}':depth-=1
        at+=1
    body=text[start:at]
    header=work/'generated-source-bus-entry.h'
    header.write_text('#include "psx_runtime.h"\n#include "psx_cyc.h"\n'+body+'\n',encoding='utf-8',newline='\n')
    print('authored generated entry calls: '+', '.join(sorted(set(re.findall(r'\b([A-Za-z_]\w*)\(',body)))))
    print('authored generated entry boundary sites='+str(body.count('psx_source_bus_boundary(')))
    print('compiler_sha256='+hashlib.sha256(Path(recompiler).read_bytes()).hexdigest())
    print('generated_entry_sha256='+hashlib.sha256(header.read_bytes()).hexdigest())
    return header


if __name__ == '__main__':
    import argparse
    import sys
    import tempfile
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recompiler',required=True)
    parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    here=Path(__file__).resolve().parents[2]/'runtime/tests'
    sys.path.insert(0,str(here))
    from test_source_bus_fixtures import run_case
    with tempfile.TemporaryDirectory() as root:
        work=Path(root)
        header=generate_probe(args.recompiler,work)
        for opt in ('-O0','-O2'):
            print(run_case(args.cc,here,opt,work,'runtime',extra_defines=(
                'PSX_TEST_GENERATED_SOURCE="'+header.as_posix()+'"',
                'PSX_TEST_GENERATED_ENTRY=func_80010000')),flush=True)
    print('generated source CPU ownership: O0/O2 PASS')
