"""Bind startup state and callback evidence to exact production."""
import hashlib,json,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[2];evidence=source.parent/'evidence';root=Path(sys.argv[1]);tested='c164fe573e1a0ac68b50f5c24189602ab9acc161'
def digest(data):return hashlib.sha256(data).hexdigest()
files=set(evidence.glob('cdda-start-*'));files.update(evidence.glob('source-cdda-start-preflight-*'));files.update(source.glob('runtime/tests/*cdda_start*'))
for name in ['cdda-start-section-contract.json','cdda-start-successor-contract.json','cdda-start-domains.json','cd-pending-full-tu-check-c164fe573e1a.json']:files.add(root/name)
for opt in ('O0','O2'):
 identities=[]
 for stem in ['cdda-start-candidate-bf7a8f55f7bd-'+opt+'-observer-v2','cdda-start-candidate-c164fe573e1a-'+opt+'-t172-v2']:
  files.add(root/(stem+'.exe'));p=root/(stem+'-receipt.json');files.add(p);identities.append(json.loads(p.read_text()))
 for key in ('wrapper_sha256','sha_source_sha256'):
  if identities[0][key]!=identities[1][key]:raise ValueError('Changed preserved component '+key)
entries=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(files) if p.is_file()]
blob=subprocess.check_output(['git','show',tested+':runtime/src/cdrom.c'],cwd=source)
results=json.loads((evidence/'cdda-start-results-v2.json').read_text());negative=json.loads((evidence/'cdda-start-negatives-v2.json').read_text());assert all(x['rejected'] for x in negative)
result=dict(schema='t172-cdda-start-evidence-receipt-v1',tested_commit=tested,production_blob_sha256=digest(blob),preserved_wrapper_and_sha=True,totals={k:sum(x[k] for x in results) for k in ['cases','rows','events','fatal']},negative_controls=len(negative),scope='Only start_source_cdda transition; track, seek, random, reset, peek and conversion dependencies are declared seams. Signed32 requested tracks and declared full storage domains; defined position arithmetic only. No actual media/CRC/transport/INTC/timing/audio/hardware/full restore claim. Fatal observer capture is test-only; production exit(2) and overflow abort preserved.',files=entries)
output=evidence/'validation-receipt-cdda-start-v1.json'
with output.open('x') as f:json.dump(result,f,indent=2)
print(json.dumps(dict(files=len(entries),totals=result['totals'],receipt=str(output),sha256=digest(output.read_bytes()))))
