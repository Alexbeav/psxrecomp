"""Check the documented coordinate edges, then compare authored traversal feedback and work against the original source."""
from pathlib import Path
import json,os,subprocess,sys
f=json.loads(Path(__file__).with_name('source_gpu_polygon_wrap_fixtures.json').read_text(encoding='utf-8'))
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
r=subprocess.run([sys.argv[1]],capture_output=True,text=True,encoding='utf-8',errors='replace',env=env,timeout=90)
assert r.returncode==0,(r.returncode,r.stdout[-400:],r.stderr)
lines=r.stdout.splitlines()
edges=[x.split() for x in lines if x.startswith('edge ')]
failed=[name for _,name,result in edges if result!='PASS']
print(f'{len(edges)-len(failed)} of {len(edges)} documented edge checks pass')
# 14 size-restriction checks, 4 range ends, 9 wrap-alike and 3 straddling stock geometries.
assert len(edges)==30 and not failed,failed
rows=[x.split() for x in lines if not x.startswith('edge ')];assert len(rows)==len(f['cases'])==144
for row,v in zip(rows,f['cases']):
 name,work1,hash1,work2,hash2=row
 assert name==v['case'] and [int(work1),int(work2)]==v['work'] and hash2==v['source_vram_sha256'],(row,v)
print('144 stock full VRAM images and288 original-source command-work values PASS')
