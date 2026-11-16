"""Freeze sector-delivery qualification inputs, observations and identities."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1])
tested='ca130ba5715d4445442bda363ea8cd7dee671d51'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-delivery-*'));files.add(evidence/'cd-sector-delivery-preflight-v1.json');files.update(source.glob('runtime/tests/*cd_delivery*'))
for name in ['cd-delivery-section-contract.json','cd-pending-full-tu-check-ca130ba5715d.json','cd-pending-full-tu-check-b72e195ee90a.json']:files.add(root/name)
for opt in ('O0','O2'):
 for stem in ['cd-delivery-candidate-b72e195ee90a-'+opt+'-observer-v3','cd-delivery-candidate-ca130ba5715d-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));files.add(root/(stem+'-receipt.json'))
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=[]
for name in ['cd-delivery-results-v1.json','cd-delivery-holdout-results-v1.json']:results+=json.loads((evidence/name).read_text())
negative=json.loads((evidence/'cd-delivery-negative-controls-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-delivery-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),totals={k:sum(x[k] for x in results) for k in ['cases','rows','callbacks','payloads']},negative_controls=len(negative),files=entries)
output=evidence/'validation-receipt-cd-delivery-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),receipt=str(output),sha256=digest(output.read_bytes()),negative_controls=len(negative))))
