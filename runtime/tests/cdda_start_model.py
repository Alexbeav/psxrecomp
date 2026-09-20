"""Independent startup state machine, including partial fatal states."""
import copy,hashlib,json
from pathlib import Path
FIELDS='reading read_delay read_min read_sec read_sect setloc_pending s_setloc_lba cdda_playing cdda_track cdda_delay cdda_data_end_pending last_valid_subq_available pending_dataready_slot source_enabled source_seeking source_position_valid source_play_track_match read_cmd mode_reg stat_reg s_source_seek_paused pending_dataready pending_dataready_stat cdda_lba source_sectors_read source_pipe_count source_pipe_at source_report_last_tens source_async_type source_async_count s_cd_timing_next_due pending_present_due s_cd_timing_pending_seq cdda_sectors_played'.split()
def expected_rows(matrix):
 for case in json.loads(Path(matrix).read_text())['cases']:
  for step,op in enumerate([dict(op='reset')]+case['operations']):
   k=op['op'];events=[];termination=0
   if k=='reset':s=dict(fields={k:0 for k in FIELDS},subq=list(range(120,132)),async_data=list(range(80,88)),pipe_sha256=hashlib.sha256(bytes((i*73+19)&255 for i in range(4704))).hexdigest(),handle=0,counters=[0]*4,fixture=[1,0,1,1,100,0,-1,list(range(160,172))])
   elif k=='set':s['fields'][op['field']]=op['value']
   elif k=='handle':s['handle']=op['value']
   elif k=='fixture':s['fixture']=[op[x] for x in ['track_count','start_lba','selected_track','audio','delay','jitter','peek_at','values']]
   elif k=='start':
    f=s['fields'];s['counters'][0]=0;requested=op['requested_track'];tc,start,track,audio,delay,jitter,peek,payload=s['fixture']
    def event(kind,args=[]):events.append(dict(kind=kind,args=args+[0]*(5-len(args)),state=copy.deepcopy(s)))
    def clear():
     event(1);s['counters'][1]+=1
     for name in ('pending_dataready','pending_dataready_stat','pending_present_due'):f[name]=0
     f['s_cd_timing_pending_seq']=2**64-1
    def run():
     if f['reading'] or f['mode_reg']&128:event(13,[2]);return 2
     f['source_async_type']=f['source_async_count']=0;clear()
     if f['cdda_playing'] and not f['source_seeking'] and not requested and not f['setloc_pending']:return 0
     event(2,[s['handle']])
     if tc<1 or tc>9:event(13,[2]);return 2
     request=min(requested,tc)
     if f['cdda_playing']:origin=f['cdda_lba']
     else:
      event(4,[f['read_min'],f['read_sec'],f['read_sect']]);origin=max(0,(f['read_min']*60+f['read_sec'])*75+f['read_sect']-150)
     target=origin
     if request:event(3,[s['handle'],request]);target=start
     elif f['setloc_pending']:target=max(0,f['s_setloc_lba'])
     event(5,[target])
     if not track:event(13,[2]);return 2
     event(6,[s['handle'],track])
     if not audio:event(13,[2]);return 2
     event(7,[origin,target,int(bool(f['stat_reg']&2)),f['s_source_seek_paused'],f['mode_reg']]);event(8,[25000])
     if delay+jitter>2147483647:event(14);return -1
     f['cdda_delay']=delay+jitter;event(9);s['counters'][2]+=1
     for name in ('reading','read_cmd','read_delay','s_cd_timing_next_due'):f[name]=0
     clear();event(10);s['counters'][3]+=1
     f.update(cdda_data_end_pending=0,cdda_playing=1,cdda_lba=target,cdda_track=track,source_seeking=1,source_sectors_read=0,source_pipe_count=0,source_pipe_at=0,source_report_last_tens=255,source_play_track_match=request if request else -1)
     for i in range(32):
      event(11,[target+i]);s['counters'][0]+=1
      if i==peek:s['subq']=payload[:];f['last_valid_subq_available']=f['source_position_valid']=1;break
     event(12,[target,150,1,1,1]);time=max(0,target+150);f['read_min']=time//4500;f['read_sec']=time//75%60;f['read_sect']=time%75
     f['setloc_pending']=0;f['s_source_seek_paused']=0;f['stat_reg']=(f['stat_reg']&31)|66
     return 0
    termination=run()
   yield dict(case_id=case['id'],step=step-1,termination=termination,state=copy.deepcopy(s),events=events)
