"""Independent streaming chains and boundary fixtures; synthetic media only."""
import copy,itertools,json,random,sys
from pathlib import Path
S=lambda k,v:dict(op='set',field=k,value=v)
def q(track=1,frame=0x10,second=2,control=1):return [control,track,3,0x14,0x25,0x36,0,0x47,second,frame,0,0]
def rec(values,ret=1,valid=1):return {'return':ret,'valid':valid,'values':values}
def base():return [S('cdda_playing',1),S('last_valid_subq_available',1),S('source_pipe_count',2),S('source_enabled',1),S('source_report_last_tens',255),dict(op='raw',seed=9901,fail_at=-1),dict(op='pipe',slot=0,seed=773),dict(op='pipe',slot=1,seed=983),dict(op='subq',values=q()),dict(op='tape',records=[rec(q())]*128)]
cases=[]
def add(name,ops):cases.append(dict(id=name,operations=ops))
for mode,track,ret,valid,control in itertools.product([0,2,4,6],[0,1,2,170,255],[0,1], [0,1],[1,0x41,2]):
 ops=base()+[S('mode_reg',mode),S('source_play_track_match',1),dict(op='subq',values=q(track)),dict(op='tape',records=[rec(q(track,control=control),ret,valid)]),dict(op='service',cycles=0)]
 add(f'subq-{mode}-{track}-{ret}-{valid}-{control}',ops)
for cycles in [0,1,451583,451584,451585,451584*63-1,451584*63,451584*64-1,451584*64,2147483647]:
 for seek in [0,1]:add(f'cap-{cycles}-{seek}',base()+[S('source_seeking',seek),dict(op='service',cycles=cycles)])
for success in [-1,0,1,14,15,16]:
 tape=[rec(q(),0,0)]*17
 if success>=0:tape[success]=rec(q())
 add(f'seek-peek-{success}',base()+[S('source_seeking',1),S('last_valid_subq_available',0),dict(op='tape',records=tape),dict(op='service',cycles=451584)])
for fail in [0,1,2,63,64]:add(f'raw-fatal-{fail}',base()+[dict(op='raw',seed=51,fail_at=fail),dict(op='service',cycles=451584*64)])
for channel in [0,1]:
 for seed in [0,1,0xFFFFFFFF,0x80000000,24679]:
  add(f'peak-{channel}-{seed}',base()+[S('mode_reg',4),dict(op='raw',seed=seed,fail_at=-1),dict(op='tape',records=[rec(q(second=channel))]*4),dict(op='service',cycles=0)])
for frame in range(256):
 add(f'report-{frame}',base()+[S('mode_reg',4),dict(op='tape',records=[rec(q(frame=frame))]*3),dict(op='service',cycles=0),S('irq_flag',0),dict(op='service',cycles=451584)])
rng=random.Random(442097)
for n in range(70):
 ops=base()+[S('mode_reg',rng.choice([0,2,4,6,255])),S('source_play_track_match',rng.choice([-1,1,2])),S('source_seeking',rng.choice([0,1])),S('source_pipe_at',rng.randrange(2)),S('source_pipe_count',rng.randrange(3)),S('source_sectors_read',rng.choice([0,4294967295])),S('cdda_sectors_played',rng.choice([0,18446744073709551615]))]
 ops+=[dict(op='active',values=[rng.randrange(256) for _ in range(4)]),dict(op='pending',values=[rng.randrange(256) for _ in range(4)]),dict(op='response',read=13,count=16,values=list(range(16))),dict(op='async',values=list(range(8))),S('source_async_type',rng.randrange(256)),S('source_async_count',rng.randrange(9))]
 records=[rec(q(rng.choice([1,2,170]),rng.randrange(256),rng.randrange(256),rng.choice([1,65,2])),rng.choice([0,1,-1]),rng.choice([0,1,-1])) for _ in range(12)]
 ops+=[dict(op='tape',records=records)]
 for turn in range(6):ops+=[S('irq_flag',rng.choice([0,0,1,4,255])),S('cd_muted',rng.choice([0,0,1,255])),S('s_source_clock',rng.choice([0,1,-1])),S('psx_cycle_count',rng.choice([0,2**64-1])),S('s_source_ready_due',rng.choice([0,100,2**64-1])),dict(op='service',cycles=rng.choice([0,1,451583,451584,903168]))]
 add(f'chain-{n}',ops)
# Keep each collector input under 2000 rows.
batch=[];rows=0;part=0
for case in cases:
 need=len(case['operations'])+1
 if rows+need>1800:
  with Path(sys.argv[1],f'cdda-service-extended-{part}-v1.json').open('x') as f:json.dump(dict(schema='t172-cdda-service-experiment-v1',cases=batch),f)
  part+=1;batch=[];rows=0
 batch.append(case);rows+=need
if batch:
 with Path(sys.argv[1],f'cdda-service-extended-{part}-v1.json').open('x') as f:json.dump(dict(schema='t172-cdda-service-experiment-v1',cases=batch),f)
print(json.dumps(dict(cases=len(cases),parts=part+1)))
