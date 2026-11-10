"""Mega Man X5 source identity and native observation boundaries, without retail assets."""
from pathlib import Path
import json,tempfile
from megamanx5 import native_span,verify_reference,PROFILE,compare_terminal_observations,BOOT,CONTROLLER_SHA
from megamanx5_admission import FRAMES,FIXED,FIRMWARE_NAME,LOCAL,SETTINGS,MOVIE_SHA,return_rows,qualify,terminal_card_evidence,verify_identity,launch_config,FINAL_CARD
from compare_ram_pages import MAGIC,PAGE_BYTES,PAGE_COUNT,page_hash
import biohazard,nymashock_admission

def rejects(call):
    try:call()
    except (ValueError,OSError):return
    raise AssertionError('invalid Mega Man X5 admission')

assert FRAMES==6347 and BOOT=='SLUS_013.34' and FIXED['original.bk2']==FIXED['m3-megamanx5-training,x.bk2']==MOVIE_SHA
assert set(LOCAL)-set(FIXED)=={'start.lua','launch-config.json','host-closure.json'} and len(FIXED)==13
assert set(SETTINGS)=={'effective-sync.json','effective-settings.json','effective-setting-values.json','effective-ports.json'}
assert PROFILE==biohazard.PROFILE and CONTROLLER_SHA!=biohazard.CONTROLLER_SHA and MOVIE_SHA!=nymashock_admission.MOVIE_SHA
assert launch_config(Path('C:/run'),'C:/bios.bin')['DispMethod']==1 and launch_config(Path('C:/run'),'C:/bios.bin')['FirmwareUserSpecifications']=={'PSX+U':'C:/bios.bin'}
for inputs,endpoint in [(10,10),(10,15),(6347,12347)]:
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
    for value in ({},dict(schema='biohazard-independent-source-v1'),dict(schema='megamanx5-independent-source-v1',source_qualification='unqualified'),
                  dict(schema='megamanx5-independent-source-v1',source_qualification='pass',movie_sha256=nymashock_admission.MOVIE_SHA,original_inputs=FRAMES),
                  dict(schema='megamanx5-independent-source-v1',source_qualification='pass',movie_sha256=MOVIE_SHA,original_inputs=FRAMES,observed_returns=FRAMES+12001,neutral_tail=12001)):
        p.write_text(json.dumps(value));rejects(lambda:verify_reference(p))
    root=Path(directory)
    for manifest in ({},dict(schema='re1-stock-control-v1'),dict(schema='nymashock-stock-control-v2',title='other',source_commit=nymashock_admission.SOURCE_COMMIT,source_tag='2.9.1',original_inputs=FRAMES,cutoff=FRAMES,role='stock-control',neutral_tail=0,firmware_key='PSX+U',firmware_sha256=FIXED[FIRMWARE_NAME]),
                     dict(schema='nymashock-stock-control-v2',title='Mega Man X5 (USA) Training X 6377M',source_commit=nymashock_admission.SOURCE_COMMIT,source_tag='2.9.1',original_inputs=FRAMES,cutoff=FRAMES,role='stock-control',neutral_tail=0,firmware_key='PSX+J',firmware_sha256=FIXED[FIRMWARE_NAME])):
        rejects(lambda:verify_identity(root,manifest,'stock-control',0))
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
header='frame\tcycle\tlag_count\tram_sha256\n'
rows=[f'0\t0\t0\t{"0"*64}\n',f'1\t123\t1\t{"1"*64}\n',f'2\t456\t1\t{"2"*64}\n']
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);p=root/'ram.tsv'
    p.write_text(header+''.join(rows));assert list(return_rows(p,2))[-1]==(2,456,1,'2'*64)
    variants=[header+''.join(rows[:-1]),header+''.join(rows)+rows[-1],
              header+rows[0]+rows[2],header+rows[0]+rows[1].replace('\t123\t','\t0\t')+rows[2],
              header+rows[0]+rows[1]+rows[2].replace('\t1\t','\t0\t'),
              header+rows[0]+rows[1].replace('\t1\t1','\t2\t1')+rows[2],
              header+rows[0]+rows[1]+rows[2][:-2]+'Z\n','frame\tclock\n'+''.join(rows)]
    for data in variants:
        p.write_text(data);rejects(lambda:list(return_rows(p,2)))
    rejects(lambda:qualify(root,root,root/'ref.json'))
    assert not (root/'ref.json').exists()
    out=root/'existing.json';out.write_bytes(b'preserve')
    rejects(lambda:qualify(root,root/'other',out));assert out.read_bytes()==b'preserve'
    stock,observed=root/'stock',root/'observed'
    a,b=[p/FINAL_CARD for p in (stock,observed)]
    for path in (a,b):path.parent.mkdir(parents=True)
    raw=b'MC'+bytes(131070)
    a.write_bytes(raw);b.write_bytes(raw)
    assert terminal_card_evidence(stock,observed)==[a,b]
    for different in (raw[:-1]+b'\1',raw[:-1],bytes(131072)):
        b.write_bytes(different);rejects(lambda:terminal_card_evidence(stock,observed))
        assert a.read_bytes()==raw
print('Mega Man X5 adapter/admission: observation counts, source/profile/identity guards, independent terminal RAM/card failures and reference overwrite rejected')
