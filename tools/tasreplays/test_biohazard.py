"""Source identity and native observation boundaries, without retail assets."""
from pathlib import Path
import json,tempfile
from biohazard import native_span,verify_reference,PROFILE,compare_terminal_observations
from compare_ram_pages import MAGIC,PAGE_BYTES,PAGE_COUNT,page_hash

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
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);run=root/'native';(run/'cards').mkdir(parents=True)
    ram=bytes(PAGE_BYTES*PAGE_COUNT);card=b'MC'+bytes(131070)
    expected_ram=root/'source-ram.bin';expected_ram.write_bytes(ram)
    expected_card=root/'source-card.mcd';expected_card.write_bytes(card)
    reference={'terminal_ram':str(expected_ram),'terminal_card1':str(expected_card)}
    actual_ram=run/'ram-frame-000001.bin';actual_ram.write_bytes(ram)
    pages=run/'ram-pages.tsv'
    pages.write_text(MAGIC+'\n'+'\t'.join(['frame','cycle']+[f'{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT)])+
                     '\n'+'\t'.join(['1','123']+[page_hash(bytes(PAGE_BYTES))]*PAGE_COUNT)+'\n')
    # Missing card must preserve the independently established RAM match.
    a,b,error=compare_terminal_observations(run,reference,1)
    assert a is True and b is False and error.startswith('card1:')
    actual_card=run/'cards/card1.mcd';actual_card.write_bytes(card)
    assert compare_terminal_observations(run,reference,1)==(True,True,None)
    actual_card.write_bytes(card[:-1]+b'\1')
    assert compare_terminal_observations(run,reference,1)==(True,False,None)
    actual_card.write_bytes(card);actual_ram.write_bytes(ram[:-1])
    a,b,error=compare_terminal_observations(run,reference,1)
    assert a is False and b is True and error.startswith('RAM:')
print('Bio Hazard adapter: observation counts, source/profile guards and independent terminal RAM/card failures pass')
