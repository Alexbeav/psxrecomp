"""Bind streaming state and PCM and callback evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='2a1bdf1675025675b7197dd038408abdbdad145f'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cdda-service-*'));files.update(evidence.glob('source-cdda-service-preflight-*'));files.update(source.glob('runtime/tests/*cdda_service*'))
for name in ['cdda-service-section-contract.json','cdda-service-domains.json','cd-pending-full-tu-check-2a1bdf167502.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cdda-service-candidate-d9afcb19b9b0-'+opt+'-observer-v2','cdda-service-candidate-2a1bdf167502-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256','dependencies_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cdda-service-results-v1.json').read_text());results+=json.loads((evidence/'cdda-service-holdout-results-v1.json').read_text());negative=json.loads((evidence/'cdda-service-negatives-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cdda-service-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events','fatal','frames']},negative_controls=len(negative),scope='Only process_source_cdda; actual accepted peek/notification/volume/clamp preserved byte-exact. Synthetic raw/SubQ, response/IRQ, BCD/MSF and PCM sink seams; 64-service cap/raw failure/missing stored SubQ exit gates preserved. Pipe index/count and defined position/delay arithmetic domains only; no full restore, actual media/CRC/transport/INTC/timing/audio hardware or quality claim.',files=entries)
output=evidence/'validation-receipt-cdda-service-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
