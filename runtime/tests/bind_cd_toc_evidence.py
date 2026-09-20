"""Bind implicit seek return/state/event evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='aa8291a9df234c56a9f55fce28c1e2198066681c'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cd-toc-*'));files.update(evidence.glob('source-cd-toc-preflight-*'));files.update(source.glob('runtime/tests/*cd_toc*'))
for name in ['cd-toc-section-contract.json','cd-toc-domains.json','cd-pending-full-tu-check-aa8291a9df23.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cd-toc-candidate-b15429704b78-'+opt+'-observer-v1','cd-toc-candidate-aa8291a9df23-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256','dependencies_sha256','seek_header_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
files.add(source/'runtime/tests/cd_head_model.py');files.add(source/'runtime/tests/cd_implicit_model.py');files.add(source/'runtime/include/cd_seek_delay.h')
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cd-toc-results-v1.json').read_text());negative=json.loads((evidence/'cd-toc-negatives-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cd-toc-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events']},negative_controls=len(negative),scope='Only source_toc_seek_cycles; actual MSF and accepted seek wrapper/header preserved. Synthetic jitter only, all20state fields unchanged. Fullsigned32head and uint64clock sentinels here do not broaden head-update domain. Bounded MSF arithmetic; no actual PRNG/command lifecycle/media/hardware/fullrestore/legal claim.',files=entries)
output=evidence/'validation-receipt-cd-toc-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
