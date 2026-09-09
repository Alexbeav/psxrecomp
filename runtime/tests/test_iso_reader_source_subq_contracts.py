"""Compare the production CUE reader with stock authored-disc Q bytes."""
from pathlib import Path
import base64,hashlib,json,os,subprocess,sys,tempfile,zlib
fixture=json.loads(Path(__file__).with_name('source_disc_subq_fixtures.json').read_text())
expected=zlib.decompress(base64.b64decode(fixture['expected_zlib_base64']))
assert hashlib.sha256(expected).hexdigest()==fixture['expected_sha256']
with tempfile.TemporaryDirectory(prefix='source-subq-') as name:
 root=Path(name)
 for segment,count,value in fixture['segments']:(root/f'{segment}.bin').write_bytes(bytes([value])*2352*count)
 (root/'authored.cue').write_text(fixture['cue'])
 (root/'positions.txt').write_text(''.join(f'{i}\n' for i in range(fixture['rows'])))
 result=subprocess.run([sys.argv[1],str(root/'authored.cue'),str(root/'native.tsv'),str(root/'positions.txt')],capture_output=True,text=True,timeout=30)
 assert result.returncode==0,(result.returncode,result.stderr)
 actual=(root/'native.tsv').read_bytes()
 assert actual==expected,('recorded-sector SubQ differs',hashlib.sha256(actual).hexdigest())
print('1500 authored recorded-sector raw SubQ values match stock BizHawk 2.3')
