"""Original-source shaded triangle complete images and command work."""
from pathlib import Path
import json,os,subprocess,sys
fixture=json.loads(Path(__file__).with_name('source_gpu_shaded_triangle_fixtures.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
result=subprocess.run([sys.argv[1]],capture_output=True,text=True,env=env,timeout=90)
assert result.returncode==0,(result.returncode,result.stderr)
rows=[line.split() for line in result.stdout.splitlines()]
assert len(rows)==len(fixture['cases'])==128
for row,case in zip(rows,fixture['cases']):
 assert row[0]==case['case'] and int(row[1])==case['work'] and row[2]==case['source_vram_sha256'],(row,case)
print('128 stock complete VRAM images and 128 command work values PASS')
