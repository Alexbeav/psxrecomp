"""Bind head-state evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='e37b39ab6c4286184922ad868a3df9b259bf9d05'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-head-*'));files.update(evidence.glob('source-cd-head-preflight-*'));files.update(source.glob('runtime/tests/*cd_head*'))
for name in ['cd-head-section-contract.json','cd-head-domains.json','cd-pending-full-tu-check-e37b39ab6c42.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cd-head-candidate-e49a31f6804f-'+opt+'-observer-v2','cd-head-candidate-e37b39ab6c42-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cd-head-results-v2.json').read_text());negative=json.loads((evidence/'cd-head-negatives-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-head-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows']},negative_controls=len(negative),scope='Only source_drive_head_update, all nine state fields observed. Unrelated globals checked by surrounding-source preservation only. Bounded signed position and uint64 clock domains, at most4096 updates. Excludes overflow, deadline wrap/nontermination and arbitrary restore; no hardware timing, full drive, caller integration or legal claim.',files=entries)
output=evidence/'validation-receipt-cd-head-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
