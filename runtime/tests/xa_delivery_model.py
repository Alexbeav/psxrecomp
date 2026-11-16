"""Independent routing state machine and accepted integer audio model."""
import copy,hashlib,json,struct
from pathlib import Path
from validate_xa4 import decode,seeded
from validate_xa_resampler import expected as resample

def pcm(samples):return hashlib.sha256(struct.pack('<'+'h'*len(samples),*samples)).hexdigest()
def expected_rows(matrix):
 for case in json.loads(Path(matrix).read_text())['cases']:
  for step,op in enumerate([dict(op='reset')]+case['operations']):
   kind=op['op'];events=[];ret=None
   if kind=='reset':
    raw=[0]*2352;delivery=[0]*8;s=dict(controls=[0]*4,stream=[255,255,255,0],history=[0]*4,active_volume=[128,0,0,128],pending_volume=[128,0,0,128])
   elif kind=='seed':raw=seeded(op['seed'],2352,255)
   elif kind=='patch':
    for i,v in op['writes']:raw[i]=v
   elif kind=='delivery':delivery=op['values'][:]
   elif kind in ('controls','stream','history'):s[kind]=op['values'][:]
   elif kind in ('active','pending'):s[kind+'_volume']=op['values'][:]
   elif kind=='classify':
    delivery=[0]*8
    if op['have_raw'] and not op.get('null_raw',0):
     delivery[0]=raw[15]
     if raw[15]==2:delivery[1:5]=raw[16:20]
   elif kind=='call':
    def event(k,args=None,samples=None,full=False):events.append(dict(kind=k,args=args or [0]*4,state=copy.deepcopy(s),pcm_sha256=pcm(samples) if samples is not None else None,samples=samples[:] if full else None))
    mode,ff,fc,mute=s['controls'];dm,file,ch,sub,coding,*_=delivery;ret=0
    if not op.get('null_raw',0) and not op.get('null_delivery',0) and mode&64 and not mute:
     event(1,[1,0,0,0])
     if dm==2 and sub&68==68:
      packed=(file<<16)|(ch<<8)|coding
      if mode&8 and (file!=ff or ch!=fc):event(12,[97,0,packed,0])
      elif coding&56:event(12,[88,0,packed,0])
      else:
       if not s['stream'][3] or s['stream'][:3]!=[file,ch,coding]:
        event(2);s['history']=[0]*4;s['stream']=[file,ch,coding,1]
       stereo=coding&1;rate=18900 if coding&4 else 37800
       event(4 if stereo else 3,[24,0,0,0]);native=[0]*8064
       s['history'],n=decode(raw[24:2328],native,s['history'],not stereo);native=native[:n*2]
       event(5,[n,stereo,0,0],native);event(6,[n,op['lba'],0,1],native);event(7,[n,rate,9408,1],native)
       output=resample(dict(id='model',samples=native,in_frames=n,sample_rate=rate,max_frames=9408));count=output['frames'];out=output['samples'][:count*2]
       event(8,[count,0,0,0],out);event(9,[count,1,0,0],out)
       vol=s['active_volume']
       for i in range(count):
        l,r=out[i*2:i*2+2];out[i*2:i*2+2]=[max(-32768,min(32767,l*vol[j]//128+r*vol[j+2]//128)) for j in range(2)]
       event(10,[count,0,0,0],out);event(6,[count,op['lba'],1,2],out);event(11,[count,1,0,0],out,True)
       event(12,[65,0,(file<<24)|(ch<<16)|(coding<<8)|((count>>5)&255),0]);ret=1
   yield dict(case_id=case['id'],step=step-1,**{'return':ret},state=copy.deepcopy(s),delivery=delivery[:],input_sha256=hashlib.sha256(bytes(raw)).hexdigest(),input_prefix=list(range(160,168)),input_suffix=list(range(192,200)),events=events)
