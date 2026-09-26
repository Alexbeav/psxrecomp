"""Replay authored source-oracle transcripts; compare all declared state bytes."""
from pathlib import Path
import base64,hashlib,json,os,struct,subprocess,sys,tempfile,zlib
exe=Path(sys.argv[1]).resolve(strict=True);kind=sys.argv[2]
fixture=json.loads(Path(__file__).with_name(f'mdec_source_{kind}_fixtures.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
sha=lambda b:hashlib.sha256(b).hexdigest()

# SPEC-PS1B-186 amendment 4: for the DMA transcripts, every guest-observable
# field must match the oracle exactly in every row, and only internal model
# fields may differ. The rows follow the transcript's operations, so an exact
# per-row match of CHCR, DICR, IRQ3 and the MDEC status also fixes the CHCR
# bit 24 clear cycle, the IRQ cycle and every status read cycle. DMA1 RAM
# landing is observed through MADR1/BCR1 at each row and the RAM read results.
OBSERVABLE={'result','ram_word_offset','status','MADR0','BCR0','CHCR0',
            'MADR1','BCR1','CHCR1','DICR','IRQ3'}
reference=None
if kind=='dma':
 reference=json.loads((Path(__file__).parent/'restricted'/'mdec_source_dma_reference.json').read_text())
 assert reference['fields']==fixture['fields']
 assert [c['name'] for c in reference['cases']]==[c['name'] for c in fixture['cases']]
# The model's implementer runs this test black-box, so by default it prints
# no oracle values: per observable field only the mismatched row count and the
# first case/mode/row index. PSX_MDEC_CONTRACT_SHOW_VALUES=1 adds the values;
# only sessions exposed to the reference model may set it.
SHOW_VALUES=os.environ.get('PSX_MDEC_CONTRACT_SHOW_VALUES')=='1'
relaxed={};observed={}

def compare_fields(case,ref_case,mode,output):
 """Count mismatches: exact on observable fields, relaxed on internal ones."""
 expected=zlib.decompress(base64.b64decode(ref_case['output_zlib_base64']))
 assert sha(expected)==case['expected_sha256']==ref_case['sha256'],case['name']
 fields=fixture['fields'];width=len(fields)*4
 if len(output)!=len(expected):
  observed.setdefault('row_count',[0,(case['name'],mode,None),None])[0]+=1
  return
 for r in range(len(expected)//width):
  want=struct.unpack_from(f'<{len(fields)}I',expected,r*width)
  got=struct.unpack_from(f'<{len(fields)}I',output,r*width)
  for name,w,g in zip(fields,want,got):
   if w==g:continue
   if name in OBSERVABLE:
    entry=observed.setdefault(name,[0,(case['name'],mode,r),(g,w)]);entry[0]+=1
   else:
    entry=relaxed.setdefault(name,[0,0])
    entry[0]+=1;entry[1]=max(entry[1],abs(((g-w+2**31)%2**32)-2**31))

with tempfile.TemporaryDirectory(prefix='mdec-source-') as temp:
 root=Path(temp)
 for i,case in enumerate(fixture['cases']):
  data=zlib.decompress(base64.b64decode(case['input_zlib_base64']))
  assert sha(data)==case['input_sha256'],case['name']
  source=root/f'{i}.input';actual=root/f'{i}.output';source.write_bytes(data)
  for mode in ('cold','checkpoint'):
   target=actual.with_suffix('.'+mode)
   mode_env=dict(env,**({'PSX_TEST_ROUNDTRIP':'1'} if mode=='checkpoint' else {}))
   # A hang guard, not a speed check: an unoptimized checkpoint round trip after
   # every step takes ~20 s alone and exceeded a 30 s bound under ctest -j load.
   run=subprocess.run([str(exe),str(source),str(target)],env=mode_env,capture_output=True,timeout=600)
   assert run.returncode==0,(case['name'],mode,run.returncode,run.stderr)
   output=target.read_bytes()
   if len(output)==case['expected_bytes'] and sha(output)==case['expected_sha256']:continue
   assert reference is not None,(case['name'],mode)
   compare_fields(case,reference['cases'][i],mode,output)
 if relaxed:
  print('relaxed internal fields (SPEC-PS1B-186 amendment 4): field rows max_abs_diff')
  for name in fixture['fields']:
   if name in relaxed:print(f'  {name} {relaxed[name][0]} {relaxed[name][1]}')
 if observed:
  print('OBSERVABLE MISMATCHES: field rows first(case mode row)')
  for name in fixture['fields']+['row_count']:
   if name not in observed:continue
   count,(cname,cmode,row),values=observed[name]
   line=f'  {name} {count} {cname} {cmode} {row}'
   if SHOW_VALUES and values:line+=f' got={values[0]:#x} oracle={values[1]:#x}'
   print(line)
  raise SystemExit(f'{kind}: observable fields differ from the oracle')
 if kind=='dma':
  cold=[(0,0,0,0),(2,0,0x1f8010f0,0x99)]
  start=[(2,0,0x1f801080,0x3000),(2,0,0x1f801084,0x10020),(2,0,0x1f801088,0x01000201)]
  # A 16-word MDEC-in block is measured behaviour, not a scope limit:
  # [ORACLE FIXTURE D11] ran block sizes 1, 8 and 16, so the model must accept it.
  measured={
   'block-size':cold+[(2,0,0x1f801084,0x10010),start[-1]],
  }
  for name,ops in measured.items():
   source=root/(name+'.input');actual=root/(name+'.output')
   source.write_bytes(b''.join(struct.pack('<4I',*op) for op in ops))
   run=subprocess.run([str(exe),str(source),str(actual)],env=env,capture_output=True,timeout=30)
   assert run.returncode==0 and b'[dma-model]' not in run.stderr and b'[mdec-source]' not in run.stderr,(name,run.returncode,run.stderr)
  # Scope guards, not hardware behaviour: not qualified: no fixture. No D fixture
  # has measured these operations, so the model must refuse them (rc 2) rather
  # than guess. D12 group c covers only a same-value DPCR write on DMA2, not a
  # changed channel 0 DPCR nibble.
  cases={
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
