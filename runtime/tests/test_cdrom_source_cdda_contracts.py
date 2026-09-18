"""Replay authored original-source CDDA transcripts with all declared bytes."""
from pathlib import Path
import base64,hashlib,json,os,struct,subprocess,sys,tempfile,zlib
exe=Path(sys.argv[1]).resolve(strict=True)
fixture=json.loads(Path(__file__).with_name('cdrom_source_cdda_fixtures.json').read_text())
sha=lambda b:hashlib.sha256(b).hexdigest()
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
with tempfile.TemporaryDirectory(prefix='cd-source-cdda-') as temp:
 root=Path(temp);tape=root/'random.tape'
 data=zlib.decompress(base64.b64decode(fixture['tape_zlib_base64']))
 assert sha(data)==fixture['tape_sha256'];tape.write_bytes(data)
 for i,case in enumerate(fixture['cases']):
  data=zlib.decompress(base64.b64decode(case['input_zlib_base64']))
  assert len(data)==case['operations']*16 and sha(data)==case['input_sha256']
  source=root/f'{i}.input';actual=root/f'{i}.output';source.write_bytes(data)
  for mode in ('cold','checkpoint'):
   target=actual.with_suffix('.'+mode)
   mode_env=dict(env,**({'PSX_TEST_ROUNDTRIP':'1'} if mode=='checkpoint' else {}))
   run=subprocess.run([str(exe),str(tape),str(source),str(target)],env=mode_env,capture_output=True,timeout=30)
   assert run.returncode==0,(case['name'],mode,run.returncode,run.stderr)
   output=target.read_bytes()
   assert len(output)==case['expected_bytes'] and sha(output)==case['expected_sha256'],(case['name'],mode)
 for name in ['scan','double-speed','active-read','restore']:
  run=subprocess.run([str(exe),str(tape),'unused','unused',name],env=env,capture_output=True,timeout=30)
  assert run.returncode==(0 if name=='restore' else 2),(name,run.returncode,run.stderr)
  if name!='restore':assert b'[CDROM]' in run.stderr,(name,run.stderr)
print('CDDA: four complete source command/state/audio transcripts and four unsupported-scope checks PASS')
