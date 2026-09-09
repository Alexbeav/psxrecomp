"""Admission and completion checks for unchanged complete controller input."""
from pathlib import Path
import hashlib
import struct
import tempfile
from dualshock_route import write_route
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
    done=dict(frame=3,input_frames=2,neutral_tail_ticks=1,controller_profile='nymashock-2.9.1-dualshock-neutral-analog',original_controller_sha256=identity['original_controller_sha256'],applied_controller_sha256=identity['expected_protocol_sha256'],expected_protocol_sha256=identity['expected_protocol_sha256'])
    assert playback_identity_matches(done,identity,1)
    for field,value in [('frame',2),('input_frames',1),('neutral_tail_ticks',0),('controller_profile','digital'),('original_controller_sha256','0'*64),('applied_controller_sha256',identity['original_controller_sha256']),('expected_protocol_sha256','0'*64)]:
        changed=dict(done);changed[field]=value
        assert not playback_identity_matches(changed,identity,1),field
    original=route.read_bytes()
    for name,offset,value in [('version',8,1),('sequence',24,2),('physical-analog',34,1),('reserved',35,1)]:
        altered=bytearray(original);altered[offset]=value
        bad=folder/(name+'.route');bad.write_bytes(altered);rejects(lambda:route_identity(bad))
    for name,data in [('short',original[:23]),('truncated',original[:-1]),('extra',original+b'\0')]:
        bad=folder/(name+'.route');bad.write_bytes(data);rejects(lambda:route_identity(bad))
    # Axis-only transitions count against the complete-state RLE capacity.
    bad=folder/'capacity.route'
    bad.write_bytes(struct.pack('<8sIIII',b'PSXRTI2\0',2,12,4097,0)+b''.join(struct.pack('<IH6B',i+1,0xffff,i%256,128,128,128,0,0) for i in range(4097)))
    rejects(lambda:route_identity(bad))
    card=folder/'card.mcd';card.write_bytes(bytes(131072))
    assert card_identity(card)['sha256']==hashlib.sha256(bytes(131072)).hexdigest()
    card.write_bytes(bytes(131071));rejects(lambda:card_identity(card))
    card.write_bytes(bytes(131073));rejects(lambda:card_identity(card))
print('Native input identity: legacy unchanged; original/protocol identities and malformed input/card rejection pass')
