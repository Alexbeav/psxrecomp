"""Bind notification state and callback evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='e525e716659a479cf82d37fcf6878fc2d5edbd63'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cdda-notification-*'));files.update(evidence.glob('source-cdda-notification-preflight-*'));files.update(source.glob('runtime/tests/*cdda_notification*'))
for name in ['cdda-notification-section-contract.json','cd-pending-full-tu-check-e525e716659a.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cdda-notification-candidate-91ca382196ac-'+opt+'-observer-v2','cdda-notification-candidate-e525e716659a-'+opt+'-t172-v2']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
files.add(root/'cdda-notification-successor-contract.json')
for opt in ('O0','O2'):
 for stem in ['cdda-notification-candidate-91ca382196ac-'+opt+'-guard-v2-abort','cdda-notification-candidate-e525e716659a-'+opt+'-t172-v2-abort']:
  files.add(root/(stem+'.exe'));files.add(root/(stem+'-receipt.json'))
files.add(evidence/'validation-receipt-cdda-notification-v1.json')
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cdda-notification-results-v2.json').read_text());negative=json.loads((evidence/'cdda-notification-negative-controls-v2.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cdda-notification-evidence-receipt-v2',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events']},negative_controls=len(negative),scope='Only source_cdda_present/queue; valid disjoint payload, type0..255 count0..8; full signed32 enabled/source-clock and uint64 time domain. Response/readiness/IRQ observation seams without reentrancy or actual INTC/latch/timing/transport/snapshot integration.',files=entries)
failure=json.loads((evidence/'cdda-notification-failure-results-v2.json').read_text())
assert all(x['rejected_before_mutation'] for x in failure['results']) and all(x['rejected'] for x in failure['negative_controls'])
result.update(failure_cases=sum(x['cases'] for x in failure['results']),failure_negative_controls=len(failure['negative_controls']),capacity_boundary='count9..UINT32_MAX abort before mutation; instrumented boundary capture is not production termination',supersedes_unaccepted_review='b9275b8b679ddc2116a259ba3e7781f32ee5027f')
output=evidence/'validation-receipt-cdda-notification-v2.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
