"""Compare authored traversal feedback and work against the original source."""
from pathlib import Path
import json,os,subprocess,sys
f=json.loads(Path(__file__).with_name('source_gpu_polygon_wrap_fixtures.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
r=subprocess.run([sys.argv[1]],capture_output=True,text=True,env=env,timeout=90)
assert r.returncode==0,(r.returncode,r.stderr)
rows=[x.split() for x in r.stdout.splitlines()];assert len(rows)==len(f['cases'])==144
for row,v in zip(rows,f['cases']):
 name,work1,hash1,work2,hash2=row
 assert name==v['case'] and [int(work1),int(work2)]==v['work'] and hash2==v['source_vram_sha256'],(row,v)
print('144 stock full VRAM images and288 original-source command-work values PASS')
