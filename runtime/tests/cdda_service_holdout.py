"""Fresh independently seeded states and chained streaming/fatal holdouts."""
import json,random,sys
from pathlib import Path
S=lambda k,v:dict(op='set',field=k,value=v)
rng=random.Random(9850147);cases=[]
for n in range(80):
 q=[rng.randrange(256) for _ in range(12)];q[0]=rng.choice([1,65,129,241]);q[1]=rng.choice([0,1,18,170,255]);records=[]
 for i in range(20):
  v=[rng.randrange(256) for _ in range(12)];v[0]=rng.choice([0,1,65,241,255]);v[1]=rng.choice([0,1,18,170,255]);records.append({'return':rng.choice([0,1,-2147483648,2147483647]),'valid':rng.choice([0,1,-1]),'values':v})
 ops=[dict(op='handle',value=rng.randrange(3)),dict(op='raw',seed=rng.randrange(2**32),fail_at=rng.choice([-1,0,1,3])),dict(op='pipe',slot=0,seed=rng.randrange(2**32)),dict(op='pipe',slot=1,seed=rng.randrange(2**32)),dict(op='subq',values=q),dict(op='tape',records=records),dict(op='active',values=[rng.randrange(256) for _ in range(4)]),dict(op='pending',values=[rng.randrange(256) for _ in range(4)]),dict(op='response',read=15,count=16,values=[rng.randrange(256) for _ in range(16)]),dict(op='async',values=[rng.randrange(256) for _ in range(8)])]
 fields=dict(cdda_playing=rng.choice([0,1,-1,2147483647]),cdda_delay=rng.choice([-451585,-1,0,1,451584]),last_valid_subq_available=rng.choice([0,1,-1]),source_position_valid=rng.choice([0,1,-1]),source_seeking=rng.choice([0,1,-1]),source_play_track_match=rng.choice([-2147483648,-1,0,1,18,170,255,2147483647]),source_pipe_count=rng.randrange(3),source_pipe_at=rng.randrange(2),source_enabled=rng.choice([0,1,-1]),source_report_last_tens=rng.choice([0,1,15,255,2**32-1]),source_async_count=rng.randrange(9),source_async_type=rng.randrange(256),mode_reg=rng.randrange(256),stat_reg=rng.randrange(256),cd_muted=rng.randrange(2),cdda_lba=rng.choice([0,1,449999]),cdda_sectors_played=rng.choice([0,2**64-1]),source_sectors_read=rng.choice([0,2**32-1]),cdda_data_end_pending=rng.choice([-1,0,1]),s_source_seek_paused=rng.randrange(256),cdda_track=rng.choice([-2147483648,2147483647]))
 ops += [S(k,v) for k,v in fields.items()]
 for i in range(4):ops += [S('irq_flag',rng.choice([0,1,255])),S('cdda_playing',rng.choice([0,1,-1])),dict(op='service',cycles=rng.choice([0,1,451584,903168]))]
 cases.append(dict(id=f'fresh-{n}',operations=ops))
for i in range(0,len(cases),35):
 with Path(sys.argv[1],f'cdda-service-holdout-{i//35}-v1.json').open('x') as f:json.dump(dict(schema='t172-cdda-service-experiment-v1',cases=cases[i:i+35]),f)
