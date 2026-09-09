"""Source identity and native observation boundaries, without retail assets."""
from pathlib import Path
import json,tempfile
from biohazard import native_span,verify_reference,PROFILE

def rejects(call):
    try:call()
    except ValueError:return
    raise AssertionError('invalid Bio Hazard admission')

for inputs,endpoint in [(10,10),(10,15),(227202,239202)]:
    records,tail=native_span(inputs,endpoint,endpoint)
    assert records==inputs and records+tail-1==endpoint
    for cut in (1,inputs-1):
        records,tail=native_span(inputs,endpoint,cut)
        assert records==cut+1 and tail==0 and records+tail-1==cut
rejects(lambda:native_span(10,15,10));rejects(lambda:native_span(10,15,14))
for values in [(0,1,1),(10,9,1),(10,15,0),(10,15,16),(10,15,True)]:
    rejects(lambda:native_span(*values))
assert PROFILE[PROFILE.index('--pad-ack-model')+1]=='nymashock-1.29.0-dualshock'
assert PROFILE[PROFILE.index('--card-model')+1]=='nymashock-1.29.0'
assert PROFILE[PROFILE.index('--legacy-card-repair')+1]=='off'
assert '--cpu-return-probe' in PROFILE and '--ram-page-probe' in PROFILE
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)/'reference.json'
    for value in ({},dict(schema='pepsiman-independent-source-v1'),dict(schema='biohazard-independent-source-v1',source_qualification='unqualified')):
        p.write_text(json.dumps(value));rejects(lambda:verify_reference(p))
print('Bio Hazard adapter: exact observation counts and source/profile admission guards pass')
