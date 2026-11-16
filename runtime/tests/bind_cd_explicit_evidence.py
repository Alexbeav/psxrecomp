"""Bind implicit seek return/state/event evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='8adf61d12a81807d1dc898efa8b86ed296efcd94'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-explicit-*'));files.update(evidence.glob('source-cd-explicit-preflight-*'));files.update(source.glob('runtime/tests/*cd_explicit*'))
for name in ['cd-explicit-section-contract.json','cd-explicit-domains.json','cd-pending-full-tu-check-8adf61d12a81.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cd-explicit-candidate-d850e47d8229-'+opt+'-observer-v1','cd-explicit-candidate-8adf61d12a81-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256','dependencies_sha256','seek_header_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
files.add(source/'runtime/tests/cd_head_model.py');files.add(source/'runtime/tests/cd_implicit_model.py');files.add(source/'runtime/include/cd_seek_delay.h')
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cd-explicit-results-v1.json').read_text());negative=json.loads((evidence/'cd-explicit-negatives-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-explicit-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events']},negative_controls=len(negative),scope='Only source_explicit_seek_cycles; actual MSF and accepted seek wrapper/header preserved. All256command bytes tested at helper boundary, not dispatcher admission. Synthetic jitter; full24field/return/events. No actual head update/speed/media; caller lifecycle outside ownership; no fullrestore/hardware/legal claim.',files=entries)
output=evidence/'validation-receipt-cd-explicit-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
