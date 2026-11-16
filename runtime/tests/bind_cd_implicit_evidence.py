"""Bind implicit seek return/state/event evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='a63da6d2d5bc44e8804a38db6f2af702b8849141'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-implicit-*'));files.update(evidence.glob('source-cd-implicit-preflight-*'));files.update(source.glob('runtime/tests/*cd_implicit*'))
for name in ['cd-implicit-section-contract.json','cd-implicit-domains.json','cd-pending-full-tu-check-a63da6d2d5bc.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cd-implicit-candidate-cab60489d74d-'+opt+'-observer-v1','cd-implicit-candidate-a63da6d2d5bc-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256','dependencies_sha256','seek_header_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
files.add(source/'runtime/tests/cd_head_model.py');files.add(source/'runtime/include/cd_seek_delay.h')
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cd-implicit-results-v1.json').read_text());negative=json.loads((evidence/'cd-implicit-negatives-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-implicit-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events']},negative_controls=len(negative),scope='Only implicit_read_seek_cycles. Actual MSF, accepted head update and seek wrapper/header byte-preserved. Synthetic jitter/speed return seams; no actual PRNG/speed policy. Bounded MSF/head position/deadline arithmetic; no overflow/nontermination/fullrestore/media/lifecycle/hardware/legal claim.',files=entries)
output=evidence/'validation-receipt-cd-implicit-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
