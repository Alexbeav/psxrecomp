"""Check complete VRAM hashes and timing against authored source-line fixtures."""
import json, os, subprocess, sys
from pathlib import Path
fixture=json.loads(Path(__file__).with_name('source_gpu_line_fixtures.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
r=subprocess.run([sys.argv[1]],input=''.join(' '.join(map(str,c['input']))+'\n' for c in fixture['cases']),capture_output=True,text=True,env=env,timeout=120)
assert r.returncode==0,(r.returncode,r.stderr)
rows=r.stdout.splitlines()
assert len(rows)==len(fixture['cases'])
for row,case in zip(rows,fixture['cases']):
    assert row==case['expected'],(case['input'],row,case['expected'])
print(f"{len(rows)} source line timing and complete VRAM hashes PASS")
