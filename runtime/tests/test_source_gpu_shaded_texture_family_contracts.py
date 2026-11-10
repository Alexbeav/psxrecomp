"""Full stock shaded textured polygon images and source command/cache work."""
from pathlib import Path
import json,os,subprocess,sys
fixture=json.loads(Path(__file__).with_name('source_gpu_shaded_texture_family_fixtures.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
result=subprocess.run([sys.argv[1]],capture_output=True,text=True,env=env,timeout=90)
assert result.returncode==0,(result.returncode,result.stderr)
rows=[line.split() for line in result.stdout.splitlines()]
assert len(rows)==len(fixture['cases'])==1536
for row,case in zip(rows,fixture['cases']):
 assert row[0]==case['case'] and [int(x) for x in row[1:-1]]==case['work'] and row[-1]==case['source_vram_sha256'],(row,case)
assert sum(len(x['work']) for x in fixture['cases'])==4608
print('1536 stock complete VRAM images and 4608 command/cache work values PASS')
