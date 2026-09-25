"""Freeze compiler-specific XA resampler compatibility evidence."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='92ad7f7138d0d50331299b9e8fba0e52af6b1b1e'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('xa-resampler-*'));files.update(source.glob('runtime/tests/*xa_resampler*'))
for name in ['xa-resampler-section-contract.json','cd-pending-full-tu-check-92ad7f7138d0.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['xa-resampler-candidate-f315dbb3079c-'+opt+'-observer-v1','xa-resampler-candidate-92ad7f7138d0-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'xa-resampler-results-v1.json').read_text());negative=json.loads((evidence/'xa-resampler-negative-controls-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-xa-resampler-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','frames']},negative_controls=len(negative),scope='Pinned GCC16.1.0 O0/O2 observations, including signed32 overflow effects reproduced with defined arithmetic. No portable baseline C or hardware accuracy claim. Disjoint buffers; caller rates and bounded synthetic frame/capacity/null domain.',files=entries)
output=evidence/'validation-receipt-xa-resampler-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
