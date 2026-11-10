"""Bind notification state and callback evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='ff24e023d651a51de0bf1042e04b6a714355e17b'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cdda-peek-*'));files.update(evidence.glob('source-cdda-peek-preflight-*'));files.update(source.glob('runtime/tests/*cdda_peek*'))
for name in ['cdda-peek-section-contract.json','cd-pending-full-tu-check-ff24e023d651.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cdda-peek-candidate-189b9fec2147-'+opt+'-observer-v1','cdda-peek-candidate-ff24e023d651-'+opt+'-t172-v1']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cdda-peek-results-v1.json').read_text());negative=json.loads((evidence/'cdda-peek-negative-controls-v1.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cdda-peek-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events']},negative_controls=len(negative),scope='Only source_cdda_peek; signed32 LBA/state/reader values, optional initialized reader output. ISO reader is a seam, not actual ISO/CRC/media/transport validation. Uninitialized accepted buffer excluded.',files=entries)
output=evidence/'validation-receipt-cdda-peek-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
