"""Bind notification state and callback evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='2fb7a715f20d6ae14d02c868fa810812fa4b9c69'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cdda-notification-*'));files.update(evidence.glob('source-cdda-notification-preflight-*'));files.update(source.glob('runtime/tests/*cdda_notification*'))
for name in ['cdda-notification-section-contract.json','cd-pending-full-tu-check-2fb7a715f20d.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cdda-notification-candidate-91ca382196ac-'+opt+'-observer-v1','cdda-notification-candidate-2fb7a715f20d-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cdda-notification-results-v1.json').read_text());negative=json.loads((evidence/'cdda-notification-negative-controls-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cdda-notification-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events']},negative_controls=len(negative),scope='Only source_cdda_present/queue; valid disjoint payload, type0..255 count0..8; full signed32 enabled/source-clock and uint64 time domain. Response/readiness/IRQ observation seams without reentrancy or actual INTC/latch/timing/transport/snapshot integration.',files=entries)
output=evidence/'validation-receipt-cdda-notification-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
