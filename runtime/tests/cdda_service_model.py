"""Independent full streaming state, dependency and PCM observation model."""
import copy,hashlib,json,struct
from pathlib import Path
PERIOD=451584
FIELDS='cdda_playing cdda_track cdda_delay cdda_data_end_pending last_valid_subq_available read_min read_sec read_sect s_source_clock source_enabled source_seeking source_position_valid source_play_track_match mode_reg stat_reg cd_muted s_source_seek_paused irq_flag cdda_lba source_sectors_read source_pipe_count source_pipe_at source_report_last_tens source_async_type source_async_count cdda_sectors_played psx_cycle_count s_source_ready_due'.split()
def digest(b):return hashlib.sha256(b).hexdigest()
def pattern(seed):return bytes(((seed+i*73)^(i>>5))&255 for i in range(2352)) if seed else bytes(2352)
def pcm(b):return list(struct.unpack('<1176h',b))
def packed(v):return struct.pack('<1176h',*v)
class Model:
 def __init__(self):
  self.s=dict(fields=dict.fromkeys(FIELDS,0),subq=[0]*12,async_data=[0]*8,pipe_hashes=[digest(bytes(2352))]*2,response=[0]*18,active_volume=[128,0,0,128],pending_volume=[128,0,0,128],handle=0,media=[0,-1,0,0,0]);self.pipe=[bytes(2352)]*2;self.tape=[];self.events=[]
 def event(self,kind,args=(),blob=None,samples=None):
  self.events.append(dict(kind=kind,args=list(args)+[0]*(5-len(args)),state=copy.deepcopy(self.s),blob_sha256=digest(blob) if blob is not None else None,bytes=list(blob) if blob is not None and len(blob)<=12 else None,samples=samples))
 def present(self,wrapped=False):
  s=self.s;f=s['fields']
  if wrapped:self.event(1)
  if not f['source_enabled'] or not f['source_async_type']:return
  self.event(13)
  if f['irq_flag'] or (f['s_source_clock'] and f['psx_cycle_count']<f['s_source_ready_due']):return
  kind=f['source_async_type'];count=f['source_async_count'];self.event(14);s['response'][0]=s['response'][1]=0
  for value in s['async_data'][:count]:
   self.event(15,[value]);s['response'][2+s['response'][1]]=value;s['response'][1]+=1
  f['source_async_type']=f['source_async_count']=0;self.event(16,[kind]);f['irq_flag']=kind
  if f['s_source_clock']:f['s_source_ready_due']=0
  self.event(17)
 def queue(self,kind,data):
  self.event(7,[kind,len(data)],bytes(data));self.s['fields']['source_async_type']=kind;self.s['fields']['source_async_count']=len(data);self.s['async_data'][:len(data)]=data;self.present()
 def peek(self,lba):
  self.event(4,[lba]);s=self.s;m=s['media'];self.event(5,[s['handle'],lba&0xffffffff,12,m[3],0]);r=self.tape[m[3]] if m[3]<len(self.tape) else {'return':0,'valid':0,'values':[0]*12};m[3]+=1
  ok=int(bool(r['return'] and r['valid'] and r['values'][0]&15==1))
  if ok:s['subq']=r['values'][:];s['fields']['source_position_valid']=s['fields']['last_valid_subq_available']=1
  self.event(6,[ok]);return ok
 def service(self,cycles):
  self.present(True);s=self.s;f=s['fields']
  if not f['cdda_playing']:return 0
  f['cdda_delay']-=cycles;n=0
  while f['cdda_delay']<=0:
   n+=1
   if n>64:self.event(18,[2]);return 2
   if f['source_seeking']:
    f['source_seeking']=0
    for i in range(1,17):
     if self.peek(f['cdda_lba']-i):break
    f['stat_reg']=(f['stat_reg']&~64)|128;f['cdda_delay']+=PERIOD;continue
   m=s['media'];self.event(2,[s['handle'],f['cdda_lba'],2352,m[2]]);raw=pattern((m[0]+m[2]*1009)&0xffffffff);ok=int(m[2]!=m[1]);m[2]+=1;self.event(3,[ok],raw)
   if not ok:self.event(18,[2]);return 2
   fresh=self.peek(f['cdda_lba'])
   if not f['last_valid_subq_available']:self.event(18,[2]);return 2
   q=s['subq']
   if fresh and f['source_play_track_match']<0:f['source_play_track_match']=q[1]
   if q[1]==170 or (f['mode_reg']&2 and f['source_play_track_match']>=0 and f['source_play_track_match']!=q[1]):
    status=f['stat_reg'];f['cdda_delay']=0;f['source_pipe_at']=0;f['cdda_playing']=0;f['s_source_seek_paused']=1;f['source_pipe_count']=0;f['source_sectors_read']=0;f['stat_reg']&=~128
    self.queue(4,[f['stat_reg'] if q[1]==170 else status]);return 0
   if fresh and f['mode_reg']&4 and f['source_report_last_tens']!=q[9]>>4:
    f['source_report_last_tens']=q[9]>>4;channel=q[8]&1;peak=min(32767,max(abs(v) for v in pcm(raw)[channel::2]))|(channel<<15)
    time=q[3:6] if q[9]&16 else q[7:10];time=time[:]
    if q[9]&16:time[1]|=128
    self.queue(1,[f['stat_reg'],q[1],q[2],*time,peak&255,peak>>8])
   at=f['source_pipe_at']
   if f['source_pipe_count']==2:
    samples=[0]*1176 if f['cd_muted'] else pcm(self.pipe[at]);self.event(8,[588],packed(samples));v=s['active_volume'];out=[]
    for l,r in zip(samples[::2],samples[1::2]):out.extend([max(-32768,min(32767,(l*v[0]//128)+(r*v[2]//128))),max(-32768,min(32767,(l*v[1]//128)+(r*v[3]//128)))])
    self.event(9,[588],packed(out));self.event(10,[588],packed(out),out);f['cdda_sectors_played']=(f['cdda_sectors_played']+1)&((1<<64)-1)
   else:f['source_pipe_count']+=1
   self.pipe[at]=raw;s['pipe_hashes'][at]=digest(raw);f['source_pipe_at']=at^1;self.event(11,[q[1]]);f['cdda_track']=(q[1]>>4)*10+(q[1]&15);f['cdda_lba']+=1;f['source_sectors_read']=(f['source_sectors_read']+1)&0xffffffff
   self.event(12,[f['cdda_lba'],150,1,1,1]);t=f['cdda_lba']+150;f['read_min']=t//4500;f['read_sec']=t//75%60;f['read_sect']=t%75;f['cdda_delay']+=PERIOD
  return 0
 def op(self,op):
  k=op['op'];self.events=[];s=self.s
  if k=='reset':self.__init__()
  elif k=='set':s['fields'][op['field']]=op['value']
  elif k=='handle':s['handle']=op['value']
  elif k=='raw':s['media'][:3]=[op['seed'],op['fail_at'],0]
  elif k=='pipe':self.pipe[op['slot']]=pattern(op['seed']);s['pipe_hashes'][op['slot']]=digest(self.pipe[op['slot']])
  elif k in ('subq','async','active','pending'):s[{'subq':'subq','async':'async_data','active':'active_volume','pending':'pending_volume'}[k]]=op['values'][:]
  elif k=='response':s['response']=[op['read'],op['count']]+op['values'][:]
  elif k=='tape':self.tape=copy.deepcopy(op['records']);s['media'][3:]=[0,len(self.tape)]
  elif k=='service':return self.service(op['cycles'])
  return 0
def expected_rows(matrix):
 for case in json.loads(Path(matrix).read_text())['cases']:
  model=Model()
  for step,op in enumerate([dict(op='reset')]+case['operations']):
   term=model.op(op);yield dict(case_id=case['id'],step=step-1,termination=term,state=copy.deepcopy(model.s),events=copy.deepcopy(model.events))
