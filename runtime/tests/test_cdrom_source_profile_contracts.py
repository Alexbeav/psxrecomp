"""Validate compiled authored source-CD admission and fail-closed cases."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

kind, executable = sys.argv[1:3]
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
def run(args):
    return subprocess.run([executable,*map(str,args)],env=env,capture_output=True,
                          text=True,encoding='utf-8',errors='replace',timeout=30)
if kind=='receipt':
    with tempfile.TemporaryDirectory(prefix='cd-source-receipt-') as name:
        tape=Path(name)/'authored-zero.psxrng'
        tape.write_bytes(b'PSX-CD-RNG1'.ljust(16,b'\0')+struct.pack('<I',64)+bytes(256))
        result=run([tape])
        assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
        print(result.stdout,end='')
elif kind=='drive':
    tape=Path(sys.argv[3])
    for case in ('bad-model','no-tape','init-cdda','init-read-seek','bad-draw-order','draw-order-no-drive'):
        result=run([tape,case])
        assert result.returncode==2 and '[CDROM]' in result.stderr,(case,result.returncode,result.stderr)
    print('PASS: six unsupported source-drive cases refuse with exit2 and a CDROM diagnostic')
else:
    raise SystemExit('unknown contract kind')
