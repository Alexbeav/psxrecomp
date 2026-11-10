"""Freeze metadata and hashes without reading excluded runtime source text."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

source=Path(__file__).resolve().parents[2]
evidence=source.parent/'evidence'
root=Path(sys.argv[1])
tested='f092e94ebc567124d775fb1f8ecdb1554918e5a4'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-pending-*'))
files.update(source.glob('runtime/tests/*cd_pending*'))
files.add(evidence/'cd-data-ready-specification-inputs-v1.json')
files.add(root/'cd-pending-section-contract.json')
files.add(root/'cd-pending-full-tu-check-f092e94ebc56.json')
files.add(root/'cd-pending-full-tu-check-98682c6b6340.json')
for opt in ('O0','O2'):
 for prefix in ('cd-pending-candidate-98682c6b6340-'+opt+'-observer-v2','cd-pending-candidate-f092e94ebc56-'+opt+'-t172-v1'):
  files.add(root/(prefix+'.exe'));files.add(root/(prefix+'-receipt.json'))
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
result=dict(schema='t172-cd-pending-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),qualification_plans=['cd-pending-plan-v1.json','cd-pending-holdout-plan-v1.json'],historical_only=['cd-pending-baseline-O0-v1.jsonl','cd-pending-edges-baseline-O0-v1.jsonl'],files=entries)
output=evidence/'validation-receipt-cd-pending-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),receipt=str(output),sha256=digest(output.read_bytes()))))
