"""Bind complete XA4 observations to exact production and test identities."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='8478bc0a082db75d66b014ede5e70e3db22864d8'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('xa4-*'));files.update(source.glob('runtime/tests/*xa4*'))
for name in ['xa4-section-contract.json','cd-pending-full-tu-check-8478bc0a082d.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['xa4-candidate-9df13f58d390-'+opt+'-observer-v1','xa4-candidate-8478bc0a082d-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('clamp_sha256','wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'xa4-results-v1.json').read_text());negative=json.loads((evidence/'xa4-negative-controls-v1.json').read_text());assert all(x['rejected'] for x in negative)
checks=json.loads((root/'cd-pending-full-tu-check-8478bc0a082d.json').read_text())
result=dict(schema='t172-xa4-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_clamp_wrapper_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','decodes','frames']},negative_controls=len(negative),scope='Only XA4 mono/stereo decoding; disjoint non-null buffers and signed24 history seeds. Full signed32 snapshot domain excluded. No hardware, reset/restore, resampling, transport or audible-gameplay claim.',files=entries)
output=evidence/'validation-receipt-xa4-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
