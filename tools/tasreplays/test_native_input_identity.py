"""Admission and completion checks for unchanged complete controller input."""
from pathlib import Path
import hashlib
import struct
import tempfile
from dualshock_route import write_route,MAX_STEPS
from run_native import route_identity,card_identity,playback_identity_matches

def rejects(call):
    try: call()
    except ValueError: return
    raise AssertionError('invalid input admitted')

with tempfile.TemporaryDirectory() as folder:
    folder=Path(folder)
    v1=folder/'digital.route'
    v1.write_bytes(struct.pack('<8sIIII',b'PSXRTI1\0',1,8,2,0)+struct.pack('<IHHIHH',1,0xfff7,0,2,0xffff,0))
    one=route_identity(v1)
    assert one=={'frames':2,'sha256':hashlib.sha256(v1.read_bytes()).hexdigest(),'words_sha256':hashlib.sha256(bytes.fromhex('f7ffffff')).hexdigest()}
    assert playback_identity_matches(dict(frame=3,input_frames=2,neutral_tail_ticks=1,applied_words_sha256=one['words_sha256']),one,1)
    route=folder/'dualshock.route'
    rows=[(0xffef,1,0,128,255,0),(0xffff,129,130,131,132,0)]
    exported=write_route(rows,route); identity=route_identity(route)
    assert identity['original_controller_sha256']==exported['canonical_controller_sha256']
    # Independent explicit source-protocol examples straddle the nonidentity conversion.
    protocol=struct.pack('<H5B',0xffef,1,0,128,254,0)+struct.pack('<H5B',0xffff,128,129,130,131,0)
    assert identity['expected_protocol_sha256']==hashlib.sha256(protocol).hexdigest()
    suffix=route_identity(route,start=1)
    assert suffix['frames']==2 and suffix['sha256']==identity['sha256']
    assert suffix['expected_protocol_sha256']==hashlib.sha256(protocol[7:]).hexdigest()
    assert suffix['original_controller_sha256']==hashlib.sha256(struct.pack('<H5B',*rows[1])).hexdigest()
    done=dict(frame=3,input_frames=2,neutral_tail_ticks=1,controller_profile='nymashock-2.9.1-dualshock-neutral-analog',original_controller_sha256=identity['original_controller_sha256'],applied_controller_sha256=identity['expected_protocol_sha256'],expected_protocol_sha256=identity['expected_protocol_sha256'])
    assert playback_identity_matches(done,identity,1)
    for field,value in [('frame',2),('input_frames',1),('neutral_tail_ticks',0),('controller_profile','digital'),('original_controller_sha256','0'*64),('applied_controller_sha256',identity['original_controller_sha256']),('expected_protocol_sha256','0'*64)]:
        changed=dict(done);changed[field]=value
        assert not playback_identity_matches(changed,identity,1),field
    # Octoshock routes: the pad receives each logged byte unscaled, under its own named profile.
    raw=route_identity(route,axis_model='octoshock-byte')
    assert raw['original_controller_sha256']==identity['original_controller_sha256'] and raw['sha256']==identity['sha256']
    assert raw['expected_protocol_sha256']==hashlib.sha256(b''.join(struct.pack('<H5B',*r) for r in rows)).hexdigest()
    assert raw['expected_protocol_sha256']!=identity['expected_protocol_sha256']
    assert raw['axis_model']=='octoshock-byte' and raw['controller_profile']=='octoshock-byte-dualshock-neutral-analog'
    assert 'controller_profile' not in identity and 'axis_model' not in identity  # recorded Nymashock identities unchanged
    assert route_identity(route,start=1,axis_model='octoshock-byte')['expected_protocol_sha256']==hashlib.sha256(struct.pack('<H5B',*rows[1])).hexdigest()
    raw_done=dict(done,controller_profile='octoshock-byte-dualshock-neutral-analog',applied_controller_sha256=raw['expected_protocol_sha256'],expected_protocol_sha256=raw['expected_protocol_sha256'])
    assert playback_identity_matches(raw_done,raw,1)
    assert not playback_identity_matches(dict(raw_done,controller_profile='nymashock-2.9.1-dualshock-neutral-analog'),raw,1)
    assert not playback_identity_matches(dict(done,controller_profile='octoshock-byte-dualshock-neutral-analog'),identity,1)
    assert not playback_identity_matches(raw_done,identity,1)
    rejects(lambda:route_identity(route,axis_model='octoshock'))
    rejects(lambda:route_identity(v1,axis_model='octoshock-byte'))
    assert route_identity(v1,axis_model='nymashock-u16')==one
    original=route.read_bytes()
    for name,offset,value in [('version',8,1),('sequence',24,2),('physical-analog',34,1),('reserved',35,1)]:
        altered=bytearray(original);altered[offset]=value
        bad=folder/(name+'.route');bad.write_bytes(altered);rejects(lambda:route_identity(bad))
        rejects(lambda:route_identity(bad,axis_model='octoshock-byte'))  # the Analog refusal is the same for both
    for name,data in [('short',original[:23]),('truncated',original[:-1]),('extra',original+b'\0')]:
        bad=folder/(name+'.route');bad.write_bytes(data);rejects(lambda:route_identity(bad))
    # Axis-only transitions count against the complete-state RLE capacity.
    bad=folder/'capacity.route'
    over=MAX_STEPS+1  # one past the shared encoder/runtime cap (was a private 4096)
    bad.write_bytes(struct.pack('<8sIIII',b'PSXRTI2\0',2,12,over,0)+b''.join(struct.pack('<IH6B',i+1,0xffff,i%256,128,128,128,0,0) for i in range(over)))
    rejects(lambda:route_identity(bad))
    card=folder/'card.mcd';card.write_bytes(bytes(131072))
    assert card_identity(card)['sha256']==hashlib.sha256(bytes(131072)).hexdigest()
    card.write_bytes(bytes(131071));rejects(lambda:card_identity(card))
    card.write_bytes(bytes(131073));rejects(lambda:card_identity(card))
print('Native input identity: legacy unchanged; Nymashock and Octoshock protocol identities and malformed input/card rejection pass')

# Run the actual runner admission, staging and completion judgement without a
# product process. Synthetic completion is deliberately not a native receipt.
from unittest.mock import patch
from types import SimpleNamespace
import json, sys
import run_native
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for name in ('player.exe', 'game.toml', 'disc.bin', 'bios.bin'):
        (root/name).write_bytes(b'fixture')
    c1=root/'one.mcd'; c2=root/'two.mcd'
    c1.write_bytes(bytes(131072)); c2.write_bytes(bytes([1])*131072)
    route=root/'fixture.psxrti'
    write_route([(0xffff,128,128,128,128,0)]*2,route)
    ident=route_identity(route,axis_model='octoshock-byte')
    base=['run_native.py',str(root/'out'),'--exe',str(root/'player.exe'), '--game',str(root/'game.toml'),
          '--disc',str(root/'disc.bin'),'--bios',str(root/'bios.bin'),'--route',str(route),
          '--pad-ack-model','nymashock-1.29.0-dualshock','--axis-model','octoshock-byte']
    for index,(cards,p2,fault) in enumerate(((0,False,''),(1,False,''),(2,True,''),(2,True,'card2'),(2,True,'p2'))):
        out=root/f'out{index}'; argv=base.copy(); argv[1]=str(out)
        if cards: argv+=['--card1',str(c1),'--card-model','nymashock-1.29.0','--legacy-card-repair','off']
        if cards==2: argv+=['--card2',str(c2)]
        if p2: argv+=['--port2-controller','dualshock-neutral']
        def budget(process,run,*args,**kwargs):
            manifest=json.loads((run/'manifest.json').read_text())
            env=manifest['psx_environment']
            settings=(run/'settings.toml').read_text()
            assert ('enable2 = true' in settings) == (cards==2)
            assert ('PSX_INPUT_ROUTE_PORT2' in env)==p2
            assert ('PSX_INPUT_ROUTE_CARD2_SHA256' in env)==(cards==2)
            observed=dict(card1_present=bool(cards),card2_present=cards==2,
                          card1_sha256=card_identity(c1)['sha256'] if cards else None,bytes=131072*cards)
            if cards: assert (run/'cards/card1.mcd').read_bytes()==c1.read_bytes()
            if cards==2:
                assert (run/'cards/card2.mcd').read_bytes()==c2.read_bytes()
                assert env['PSX_INPUT_ROUTE_CARD2_SHA256']==card_identity(c2)['sha256']
                observed['card2_sha256']=card_identity(c2)['sha256'] if fault!='card2' else '0'*64
            (run/'initial-cards.json').write_text(json.dumps(observed))
            done=dict(frame=2,input_frames=2,neutral_tail_ticks=0,
                      controller_profile=ident['controller_profile'],original_controller_sha256=ident['original_controller_sha256'],
                      expected_protocol_sha256=ident['expected_protocol_sha256'],applied_controller_sha256=ident['expected_protocol_sha256'])
            if p2 and fault!='p2': done['port2_controller']='dualshock-neutral'
            (run/'complete.json').write_text(json.dumps(done))
            return dict(exit_code=0,stop_reason=None,last_inventory={})
        with patch.object(sys,'argv',argv), patch('subprocess.Popen',return_value=SimpleNamespace(pid=123)), patch('process_budget.wait_budgeted',side_effect=budget):
            assert run_native.main()==(1 if fault else 0), (cards,p2,fault)
    argv=base+['--card2',str(c2)]
    with patch.object(sys,'argv',argv): rejects(run_native.main)
print('Runner synthetic main-path checks: P1/no-card, single-card, two-card/neutral-P2 staging and independent completion failures passed; no product started')
