"""Bind reset state and callback evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='d8db2565282cab2d5a5065e63da7f6a0c4116c4f'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-reset-*'));files.update(evidence.glob('source-cd-reset-preflight-*'));files.update(source.glob('runtime/tests/*cd_reset*'))
for name in ['cd-reset-section-contract.json','cd-reset-domains.json','cd-pending-full-tu-check-d8db2565282c.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cd-reset-candidate-5c4f7374ab99-'+opt+'-observer-v1','cd-reset-candidate-d8db2565282c-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256','dependencies_sha256','seek_header_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
files.add(source/'runtime/tests/cd_head_model.py');files.add(source/'runtime/include/cd_seek_delay.h')
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cd-reset-results-v4.json').read_text());negative=json.loads((evidence/'cd-reset-negatives-v3.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-reset-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events']},negative_controls=len(negative),scope='Only process_source_reset. Actual accepted head helper preserved. Synthetic readiness and recording-only response/IRQ/SPU/XA callbacks; full27field/result/event equivalence. Fulluint64 incoming head due safe because preserved or overwritten; clock/reset_due and catch-up bounded. No actual IRQ/audio/XA internals, initiation/process_pending ownership, media/hardware/fullrestore/legal claim',files=entries)
output=evidence/'validation-receipt-cd-reset-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
