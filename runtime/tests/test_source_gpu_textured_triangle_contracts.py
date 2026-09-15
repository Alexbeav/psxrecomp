"""Compare every complete native VRAM image hash to authored stock-source cases."""
import json,os,subprocess,sys
from pathlib import Path
fixture=json.loads(Path(__file__).with_name('source_gpu_textured_triangle_fixtures.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
r=subprocess.run([sys.argv[1]],capture_output=True,text=True,env=env,timeout=90)
assert r.returncode==0,(r.returncode,r.stderr)
rows=[line.split() for line in r.stdout.splitlines()]
assert len(rows)==len(fixture['cases'])==384
for row,case in zip(rows,fixture['cases']):assert row==[case['case'],case['source_sha256']],(row,case['case'])
print(r.stderr.strip());print('384 complete stock Octoshock2.3 VRAM hashes PASS')
