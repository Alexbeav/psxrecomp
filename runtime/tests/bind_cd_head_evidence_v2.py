"""Bind head-state evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='5f3f8dd6a26e2991d0e3f8b7120c0f39872ce4aa'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-head-*'));files.update(evidence.glob('source-cd-head-preflight-*'));files.update(source.glob('runtime/tests/*cd_head*'))
for name in ['cd-head-section-contract.json','cd-head-domains.json','cd-head-successor-section-contract.json','cd-pending-full-tu-check-5f3f8dd6a26e.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cd-head-candidate-e49a31f6804f-'+opt+'-observer-v2','cd-head-candidate-5f3f8dd6a26e-'+opt+'-t172-v2']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cd-head-results-v3.json').read_text());negative=json.loads((evidence/'cd-head-negatives-v2.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-head-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows']},negative_controls=len(negative),scope='Only source_drive_head_update, all nine state fields observed. Unrelated globals checked by surrounding-source preservation only. Full signed32 target when hold0; hold nonzero target<=INT32_MAX-2. Wide threshold arithmetic, bounded LBA and uint64 clock domains, at most4096 updates. Excludes overflow, deadline wrap/nontermination and arbitrary restore; no hardware timing, full drive, caller integration or legal claim.',files=entries)
output=evidence/'validation-receipt-cd-head-v2.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
