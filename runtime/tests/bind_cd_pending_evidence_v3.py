"""Bind v3 qualification and preserve the provisional evidence history."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path
source=Path(__file__).resolve().parents[2]; evidence=source.parent/'evidence'; root=Path(sys.argv[1])
tested='43a01a4c0dbfe6b3f5df2fae8142dfe001f6dadf'
def digest(data):return hashlib.sha256(data).hexdigest()
previous=json.loads((evidence/'validation-receipt-cd-pending-v1.json').read_text())
for item in previous['files']:
    if digest(Path(item['path']).read_bytes())!=item['sha256']:raise ValueError('Historical evidence changed: '+item['path'])
files={Path(item['path']) for item in previous['files']}
files.add(evidence/'validation-receipt-cd-pending-v1.json')
files.update(evidence.glob('cd-pending-*'));files.update(source.glob('runtime/tests/*cd_pending*'))
for name in ['cd-pending-section-contract-37b1b11f.json','cd-pending-full-tu-check-43a01a4c0dbf.json','cd-pending-restore-domain-v1.json','cd-pending-restore-domain-98682c6b6340.jsonl','cd-pending-restore-domain-f092e94ebc56.jsonl']:
    files.add(root/name)
for opt in ('O0','O2'):
    for stem in ['cd-pending-candidate-98682c6b6340-'+opt+'-observer-v3','cd-pending-candidate-43a01a4c0dbf-'+opt+'-t172-v3']:
        files.add(root/(stem+'.exe'));files.add(root/(stem+'-receipt.json'))
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
result=dict(schema='t172-cd-pending-evidence-receipt-v3',tested_commit=tested,production_blob_sha256=digest(blob),qualification_plan='cd-pending-plan-v3.json',superseded_provisional_receipt='validation-receipt-cd-pending-v1.json',files=entries)
output=evidence/'validation-receipt-cd-pending-v3.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),receipt=str(output),sha256=digest(output.read_bytes()))))
