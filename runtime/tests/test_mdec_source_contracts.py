"""Replay authored source-oracle transcripts; compare all declared state bytes."""
from pathlib import Path
import base64,hashlib,json,os,struct,subprocess,sys,tempfile,zlib
exe=Path(sys.argv[1]).resolve(strict=True);kind=sys.argv[2]
fixture=json.loads(Path(__file__).with_name(f'mdec_source_{kind}_fixtures.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
sha=lambda b:hashlib.sha256(b).hexdigest()
with tempfile.TemporaryDirectory(prefix='mdec-source-') as temp:
 root=Path(temp)
 for i,case in enumerate(fixture['cases']):
  data=zlib.decompress(base64.b64decode(case['input_zlib_base64']))
  assert sha(data)==case['input_sha256'],case['name']
  source=root/f'{i}.input';actual=root/f'{i}.output';source.write_bytes(data)
  run=subprocess.run([str(exe),str(source),str(actual)],env=env,capture_output=True,timeout=30)
  assert run.returncode==0,(case['name'],run.returncode,run.stderr)
  output=actual.read_bytes()
  assert len(output)==case['expected_bytes'] and sha(output)==case['expected_sha256'],case['name']
 if kind=='dma':
  cold=[(0,0,0,0),(2,0,0x1f8010f0,0x99)]
  start=[(2,0,0x1f801080,0x3000),(2,0,0x1f801084,0x10020),(2,0,0x1f801088,0x01000201)]
  cases={
   'capture-mdec':cold+[(11,0,0,0)],'capture-dma':cold+[(12,0,0,0)],
   'block-size':cold+[(2,0,0x1f801084,0x10010),start[-1]],
   'reverse':cold+start[:2]+[(2,0,0x1f801088,0x01000203)],
   'wrong-direction':cold+start[:2]+[(2,0,0x1f801088,0x01000200)],
   'active-replace':cold+start+[(2,0,0x1f801080,0x4000)],
   'active-control':cold+start+[(2,0,0x1f8010f0,0x90)],
  }
  for name,ops in cases.items():
   source=root/(name+'.input');actual=root/(name+'.output')
   source.write_bytes(b''.join(struct.pack('<4I',*op) for op in ops))
   run=subprocess.run([str(exe),str(source),str(actual)],env=env,capture_output=True,timeout=30)
   assert run.returncode==2 and (b'[dma-model]' in run.stderr or b'[mdec-source]' in run.stderr),(name,run.returncode,run.stderr)
print(f"{kind}: {len(fixture['cases'])} complete source-oracle transcripts PASS")
