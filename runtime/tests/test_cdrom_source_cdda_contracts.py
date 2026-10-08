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
 # Scope. Where the source profile is not qualified the controller stops the process
 # with exit status 2. The fixture reports the step that was running (0 setup, 1 the
 # step under test, 2 the step returned) and each controller value before the step
 # and at the end. Nothing is read from stderr.
 def scope(name,tape_path=tape):
  report=root/f'scope-{name}-{tape_path.name}.report'
  run=subprocess.run([str(exe),str(tape_path),'unused',str(report),name],env=env,capture_output=True,timeout=30)
  rows=[line.split() for line in report.read_text().splitlines()]
  assert rows[0][0]=='step' and len(rows)==12,(name,rows)
  return run.returncode,int(rows[0][1]),{key:(int(before),int(after)) for key,before,after in rows[1:]}
 # Accepted twins: the step returns, takes one draw and changes the controller.
 code,step,v=scope('single-speed')
 assert (code,step)==(0,2) and v['draws'][1]==v['draws'][0]+1,('single-speed',code,step,v)
 assert v['mode'][0]==0 and v['reading'][0]==0 and v['cdda_playing']==(0,1) and v['cdda_seeking']==(0,1),('single-speed',v)
 code,step,v=scope('getstat')
 assert (code,step)==(0,2) and v['draws'][1]==v['draws'][0]+1 and v['command_pending']==(0,1),('getstat',code,step,v)
 # Refused: the stop comes from the step under test, before a draw and before any change.
 for name,key,value in [('double-speed','mode',0x80),('active-read','reading',1),('scan-forward','mode',0),('scan-backward','mode',0)]:
  code,step,v=scope(name)
  assert (code,step)==(2,1) and v[key][0]==value,(name,code,step,v)
  assert all(before==after for before,after in v.values()),(name,v)
 # A stop during setup has the same exit status. The report tells it apart.
 code,step,v=scope('double-speed',root/'missing.tape')
 assert (code,step)==(2,0),('setup stop',code,step,v)
 run=subprocess.run([str(exe),str(tape),'unused','unused','restore'],env=env,capture_output=True,timeout=30)
 assert run.returncode==0,('restore',run.returncode,run.stderr)
print('CDDA: four complete source command/state/audio transcripts, four refused scope cases with two accepted twins, and the null restore PASS')
