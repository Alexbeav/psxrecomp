"""Freeze sample qualification, preserved clamp identities and immutable evidence."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='1d055ad6e7294f74dde0956cdb816f1d08081403'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-volume-*'));files.add(evidence/'cd-audio-volume-preflight-v1.json');files.update(source.glob('runtime/tests/*cd_volume*'))
for name in ['cd-volume-section-contract.json','cd-pending-full-tu-check-1d055ad6e729.json','cd-pending-full-tu-check-86b1279fe44d.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cd-volume-candidate-86b1279fe44d-'+opt+'-observer-v2','cd-volume-candidate-1d055ad6e729-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('clamp_sha256','wrapper_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cd-volume-results-v1.json').read_text());negative=json.loads((evidence/'cd-volume-negative-controls-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-volume-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_clamp_and_wrapper=True,selected_header_binding='Per-side builder identity; header contains the owned region and therefore changes with it',totals={k:sum(x[k] for x in results) for k in ['cases','rows','applications','frames']},negative_controls=len(negative),files=entries)
output=evidence/'validation-receipt-cd-volume-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),receipt=str(output),sha256=digest(output.read_bytes()))))
