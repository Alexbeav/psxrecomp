"""Bind complete XA routing state, callbacks and actual accepted audio pipeline."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='986f6e135a74c58b62d197388f492f30291b8934'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('xa-delivery-*'));files.update(evidence.glob('xa-routing-preflight-*'));files.update(source.glob('runtime/tests/*xa_delivery*'))
for name in ['validate_xa4.py','validate_xa_resampler.py']:files.add(source/'runtime/tests'/name)
for name in ['xa-delivery-section-contract.json','cd-pending-full-tu-check-986f6e135a74.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['xa-delivery-candidate-14d1cb79a286-'+opt+'-observer-v1','xa-delivery-candidate-986f6e135a74-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','dependencies_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'xa-delivery-results-v1.json').read_text());negative=json.loads((evidence/'xa-delivery-negative-controls-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-xa-delivery-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_dependencies_and_wrapper=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events','frames']},negative_controls=len(negative),scope='Only maybe_deliver_xa_audio. Actual unchanged classifier/predicate/reset and accepted decoder/resampler/volume, observation seams for scan/trace/sink. Signed24 seeded histories; no full restore, actual sink/IRQ/timing/transport or hardware claim.',files=entries)
output=evidence/'validation-receipt-xa-delivery-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
