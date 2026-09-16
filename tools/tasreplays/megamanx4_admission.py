"""Admit the independently replayed Mega Man X4 Nymashock source, never native output.

Publication 6790M, "X, no items", a native BizHawk 2.10 Nymashock recording, replayed on
the pinned 2.10 host through source_control_nyma210.py with the observer-291-generic source
compiled against the 2.10 assemblies. (6412M, the 2.8 recording this lane started with,
plays its inputs on the 2.9.1 core but desyncs before frame 14,400 and was rejected on
2026-09-16: a 2.8 recording does not sync on a 2.9.1 core.) Structure and every check are
the Mega Man X5 module's, including the movie-header firmware cross-check.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import zipfile
from itertools import zip_longest
from observation_evidence import captured_frames, compare_stock_observations, terminal_consistency
from compare_ram_pages import read_pages

FRAMES=143342
SOURCE_COMMIT='dd232820493c05296c304b64bf09c57ff1e4812f'
MOVIE_SHA='77837dcb28d457b18bc5e5d827cd5d540875d494310bd4f8ed0725e308ab215f'
CARD_SHA='78b6d4ac9ab4d23caf7e5f04f83539bf5d994cccfb0a709d14ac53d05c8e21ef'
FINAL_CARD="SaveRAM/Mega Man X4 (USA).SaveRAM"
TITLE='Mega Man X4 (USA) X no items 6790M'
FIRMWARE_KEY='PSX+U'
# The firmware this movie's own header declares. Its header says
# PSX_Firmware_U 0555C6FAE8906F3F09BAF5988F00E55F88E9F30B, which is SCPH-5501;
# admission cross-checks that SHA-1 against the loaded BIOS below, so this pin
# and the movie can never drift apart again.
FIRMWARE_NAME='SCPH5501.BIN'
FIRMWARE_SHA1='0555c6fae8906f3f09baf5988f00e55f88e9f30b'
FIXED={
    'Mega Man X4 (USA).bin':'3ceab06ac99add4035f912188bcbf5b16c02056f46a33d38ff0c8dbce6cb613b',
    'Mega Man X4 (USA).cue':'d8cb97b968aba5cae059540456f85441933c2a8c213df5a3064d2d28e9eef8bd',
    'BizHawk-2.10-win-x64.zip':'fdd0e7ae57afcb04509861408fdbb499bec52cffcc7b008526b60d3548a872ec',
    'EmuHawk.exe':'a23eccb289d1a09b8b9ca09677718725acebab7d610e4b0ee93f7daf54064a25',
    'BizHawk.Emulation.Cores.dll':'54fa9f589b3784042a37c4cffce88b26e23d5243bb49c01bd451dc8bcf035744',
    'shock.wbx.zst':'7fe6d288593b1bec65fcef0e26ccadec745c25693abb0a7ad0f0b27bdf4fd627',
    FIRMWARE_NAME:'11052b6499e466bbf0a709b1f9cb6834a9418e66680387912451e971cf8a1fef',
    'happyleev2-mmx4-x_noitems.bk2':MOVIE_SHA,'original.bk2':MOVIE_SHA,
    'Observation291.dll':'d2ecbe85fdd25754d97db7085ad4d7f2ab79e18900378ae30188bbaa744e9ade',
    'source_control_nyma210.py':'01a9bcfe5b15be23a99c4e1831481ba763ade34e8d080f26496b0f484fc955f2',
    'source_control_nyma.lua':'cc44363aff7c645e427764d3e5eeed8256e166ce517b6a310e9097bffbe34f4c',
    'observer-build.json':'cab11a1ecb70fd79779b0565d7134267f372efbea1285a3ebe821cb6ddfb8e9a',
}
# Run-local artifacts the launcher writes or copies beside the run's manifest.
LOCAL=('original.bk2','source_control_nyma210.py','source_control_nyma.lua','observer-build.json','start.lua','launch-config.json','host-closure.json')
# Unbound diagnostic evidence: controller.tsv is the host-side joypad.get() log (neutral during
# playback) and host-dialogs.jsonl records the dismissed informational end-of-movie cycle warning.
OPTIONAL=('controller.tsv','host-dialogs.jsonl')
# Canonical JSON identities from the qualified source prefix's resolved defaults.
SETTINGS={
    'effective-sync.json':'066f68ba9ee8aa8be5753dd8b69056bf479b2fea00e4c22f5072e58621e96f97',
    'effective-settings.json':'9f78b1634dac17e574889add799f181c3f37f7ac8508bf92e5b9cae964652ad3',
    'effective-setting-values.json':'1decca016c7e5a083b39e57fa29bd1d781eda141305fd9a91dedad855cd2f4bf',
    'effective-ports.json':'e3ceec7164b05978e42d0ad200027a700f4a0ec607f1630ea2b800d72444168c',
}

def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def read(path):return json.loads(Path(path).read_text())

def bind(path):return {'path':str(Path(path).resolve()),'sha256':digest(path)}

def card_bytes(path):
    data=Path(path).read_bytes()
    if len(data)!=131072 or data[:2]!=b'MC':raise ValueError('invalid persisted raw source card')
    return data

def terminal_card_evidence(stock,observed):
    paths=[Path(root)/FINAL_CARD for root in (stock,observed)]
    if card_bytes(paths[0])!=card_bytes(paths[1]):raise ValueError('source observer persisted card differs')
    return paths

def return_rows(path,endpoint):
    """Validate every independent full-RAM digest and public master clock."""
    with Path(path).open() as stream:
        if stream.readline().rstrip('\r\n')!='frame\tcycle\tlag_count\tram_sha256':
            raise ValueError('wrong source RAM/clock columns')
        previous=-1; previous_lag=0; count=0
        for expected,line in enumerate(stream):
            row=line.rstrip('\r\n').split('\t')
            if len(row)!=4 or int(row[0])!=expected or not re.fullmatch('[0-9A-Fa-f]{64}',row[3]):
                raise ValueError('invalid/discontinuous source RAM return')
            frame,cycle,lag=map(int,row[:3])
            if cycle<=previous or lag<previous_lag or lag>frame or (frame==0 and (cycle!=0 or lag!=0)):
                raise ValueError('invalid source clock/lag sequence')
            previous,previous_lag=cycle,lag;count+=1
            yield frame,cycle,lag,row[3]
        if count!=endpoint+1:raise ValueError('incomplete source RAM return coverage')

def launch_config(root,firmware):
    """The launcher's host configuration as written before EmuHawk rewrote host-config.json."""
    return {'LastWrittenFrom':'2.10','LastWrittenFromDetailed':'Version 2.10','FirmwareUserSpecifications':{FIRMWARE_KEY:firmware},
            'TargetZoomFactors':{'PSX':2},'StartPaused':True,'RunInBackground':True,'AcceptBackgroundInput':False,'SingleInstanceMode':False,
            'SoundEnabled':False,'Screenshot_CaptureOSD':False,'AutosaveSaveRAM':False,'DispMethod':1,
            'Movies':{'MovieEndAction':3,'EnableBackupMovies':False},
            'PathEntries':{'Paths':[{'System':'PSX','Type':t,'Path':str(root/d)} for t,d in [('Save RAM','SaveRAM'),('Savestates','State'),('Screenshots','Screenshots')]]}}

def declared_firmware(movie):
    """The BIOS SHA-1 the movie's own header names, and the region key it names it under.

    A .bk2 records the firmware its author ran, e.g. `PSX_Firmware_U <sha1>`. Nothing in
    this tool used to read it, so a source could be admitted on a BIOS the movie never
    used: both US titles were, for months, on SCPH1001 while every US movie declares
    SCPH-5501. Both facts were already in files admission reads.
    """
    with zipfile.ZipFile(movie) as archive:
        header=archive.read('Header.txt').decode('utf-8','replace')
    named=[l.split(' ',1) for l in header.splitlines() if l.startswith('PSX_Firmware_')]
    if len(named)!=1:raise ValueError('movie declares %d firmwares, expected exactly one'%len(named))
    return named[0][0],named[0][1].strip().lower()

def verify_identity(root,manifest,role,tail):
    if (manifest.get('schema'),manifest.get('title'),manifest.get('source_commit'),manifest.get('source_tag'),
        manifest.get('original_inputs'),manifest.get('cutoff'),manifest.get('role'),manifest.get('neutral_tail'),
        manifest.get('firmware_key'),manifest.get('firmware_sha256')) != (
        'nymashock-stock-control-v2',TITLE,SOURCE_COMMIT,'2.10',FRAMES,FRAMES,role,tail,FIRMWARE_KEY,FIXED[FIRMWARE_NAME]):
        raise ValueError('source manifest role/core/firmware/input boundary differs')
    bindings=manifest['bindings'];names={Path(x['path']).name:x for x in bindings}
    if len(names)!=len(bindings) or set(names)!=set(FIXED)|{'start.lua','launch-config.json','host-closure.json'}:
        raise ValueError('source binding closure differs')
    for name,item in names.items():
        if item['sha256']!=FIXED.get(name,item['sha256']) or digest(item['path'])!=item['sha256']:
            raise ValueError('changed source artifact: '+name)
        if name in LOCAL:
            if Path(item['path']).resolve()!=root/name:raise ValueError('run-local source artifact escapes: '+name)
    closure=read(root/'host-closure.json')
    if not closure or len({x['path'] for x in closure})!=len(closure):raise ValueError('invalid host closure')
    app=Path(names['EmuHawk.exe']['path']).resolve()
    with zipfile.ZipFile(names['BizHawk-2.10-win-x64.zip']['path']) as archive:
        for item in closure:
            path=Path(item['path']).resolve()
            if path.relative_to(app.parent).as_posix()!=item['archive_member']:
                raise ValueError('host closure archive member differs')
            if digest(path)!=item['sha256'] or hashlib.sha256(archive.read(item['archive_member'])).hexdigest()!=item['sha256']:
                raise ValueError('changed stock host closure')
    actual={p.resolve() for p in app.parent.rglob('*') if p.is_file() and
            (p.suffix.lower() in {'.exe','.dll','.wbx','.config'} or p.name.lower().endswith('.wbx.zst'))}
    if actual!={Path(x['path']).resolve() for x in closure}:raise ValueError('incomplete host artifact closure')
    expected_lua=('CONTROL_ROOT=[['+root.as_posix()+']]\nCONTROL_LENGTH='+str(FRAMES)+'\nCONTROL_END='+str(FRAMES)+'\nCONTROL_TAIL='+str(tail)+
        '\nCONTROL_HELPER=[['+Path(names['Observation291.dll']['path']).as_posix()+']]\nCONTROL_CORE_SHA="'+
        FIXED['BizHawk.Emulation.Cores.dll']+'"\nCONTROL_WBX_SHA="'+FIXED['shock.wbx.zst']+
        '"\nCONTROL_WITH_PAGES='+str(role=='pages-observer').lower()+'\nCONTROL_BIOS_SHA="'+FIXED[FIRMWARE_NAME]+'"\n'+
        (root/'source_control_nyma.lua').read_text())
    if (root/'start.lua').read_text()!=expected_lua:raise ValueError('source launch script differs')
    if read(root/'launch-config.json')!=launch_config(root,names[FIRMWARE_NAME]['path']):raise ValueError('source launch configuration differs')
    expected_command=[str(app),str(Path(names['Mega Man X4 (USA).cue']['path'])),
        '--config='+str(root/'host-config.json'),'--movie='+str(root/'original.bk2'),'--lua='+str(root/'start.lua')]
    if manifest['command']!=expected_command:raise ValueError('source command differs')
    loaded=read(root/'loaded-core.json')
    if (loaded.get('type'),loaded.get('clock'),loaded.get('assembly_sha256'),loaded.get('waterbox_sha256')) != (
        'BizHawk.Emulation.Cores.Sony.PSX.Nymashock','public WaterboxCore.CycleCount',FIXED['BizHawk.Emulation.Cores.dll'],FIXED['shock.wbx.zst']):
        raise ValueError('loaded Nymashock core differs')
    for field,name in [('assembly','BizHawk.Emulation.Cores.dll'),('waterbox','shock.wbx.zst')]:
        if Path(loaded[field]).resolve()!=Path(names[name]['path']).resolve():raise ValueError('loaded core path differs')
    if read(root/'loaded-bios.json')!={'sha256':FIXED[FIRMWARE_NAME],'bytes':524288}:
        raise ValueError('loaded BIOS differs')
    # The BIOS must be the one the movie itself declares, not merely the one pinned
    # above: pinning alone cannot catch a pin that was wrong to begin with.
    region,declared=declared_firmware(root/'original.bk2')
    if region!='PSX_Firmware_'+FIRMWARE_KEY.rsplit('+',1)[1]:
        raise ValueError('movie firmware region %s does not match %s'%(region,FIRMWARE_KEY))
    if declared!=FIRMWARE_SHA1:raise ValueError('movie declares firmware '+declared+', pinned '+FIRMWARE_SHA1)
    if hashlib.sha1(Path(names[FIRMWARE_NAME]['path']).read_bytes()).hexdigest()!=declared:
        raise ValueError('loaded BIOS is not the firmware the movie declares')
    for name,expected in SETTINGS.items():
        actual=hashlib.sha256(json.dumps(read(root/name),sort_keys=True,separators=(',',':')).encode()).hexdigest()
        if actual!=expected:raise ValueError('effective source setting differs: '+name)
    cards=read(root/'initial-cards.json')
    if cards!=[{'domain':'Memcard 1','bytes':131072,'sha256':CARD_SHA,'file':'initial-Memcard-1.bin'}] or digest(root/'initial-Memcard-1.bin')!=CARD_SHA:
        raise ValueError('source initial card differs')
    policy=read(root/'effective-run-policy.json')
    if policy.get('read_only') is not True or policy.get('movie_end_action')!='Finish' or Path(policy['save_ram_directory']).resolve()!=root/'SaveRAM':
        raise ValueError('source input/end/card isolation policy differs')

def admit(root,role):
    root=Path(root).resolve(strict=True)
    complete,end,manifest,review=[read(root/name) for name in ('complete.json','exit.json','manifest.json','semantic-review.json')]
    tail=complete.get('neutral_tail')
    if type(tail) is not int or not 0<=tail<=12000:raise ValueError('invalid declared source tail')
    endpoint=FRAMES+tail
    if (complete.get('frame'),complete.get('original_inputs'),complete.get('full_movie'))!=(endpoint,FRAMES,True):
        raise ValueError('full original source movie required')
    if end.get('exit_code')!=0 or end.get('stop_reason') is not None or end.get('input_completion') is not True or end.get('original_movie_unchanged') is not True:
        raise ValueError('source did not complete cleanly with unchanged input')
    frame=review.get('frame')
    if review.get('completion_observed') is not True or type(frame) is not int or not FRAMES<=frame<=endpoint or not review.get('witness','').strip():
        raise ValueError('source completion has not been witnessed')
    if review.get('image')!=f'frame-{frame:06d}.png' or frame not in captured_frames(endpoint,FRAMES) or digest(root/review['image'])!=review.get('image_sha256'):
        raise ValueError('source ending image differs')
    verify_identity(root,manifest,role,tail)
    terminal=None
    for terminal in return_rows(root/'ram-frames.tsv',endpoint):pass
    evidence=[root/name for name in ['complete.json','exit.json','manifest.json','semantic-review.json','ram-frames.tsv','loaded-core.json','loaded-bios.json','initial-cards.json','initial-Memcard-1.bin','effective-run-policy.json',*SETTINGS,review['image']]]
    evidence+=[root/name for name in OPTIONAL if (root/name).is_file()]
    return endpoint,terminal,evidence

def qualify(stock,observed,output):
    stock,observed=Path(stock).resolve(),Path(observed).resolve()
    if stock==observed:raise ValueError('stock and observer must be independent runs')
    output=Path(output)
    if output.exists():raise ValueError('never overwrite an admitted reference')
    endpoint,terminal,evidence=admit(stock,'stock-control')
    other,last,more=admit(observed,'pages-observer')
    if other!=endpoint or last!=terminal:raise ValueError('source terminal boundary differs')
    evidence+=more+compare_stock_observations(stock,observed,endpoint,FRAMES)
    for name in ['loaded-core.json','initial-cards.json','initial-Memcard-1.bin','effective-ports.json','effective-setting-values.json']:
        if (stock/name).read_bytes()!=(observed/name).read_bytes():raise ValueError('source observer identity differs: '+name)
    pages=observed/'ram-pages.tsv';raw=observed/f'ram-frame-{endpoint:06d}.bin'
    rows=return_rows(stock/'ram-frames.tsv',endpoint);next(rows)
    for a,b in zip_longest(rows,read_pages(pages)):
        if a is None or b is None or a[:2]!=b[:2]:raise ValueError('source page clocks/coverage differ')
    terminal_consistency(pages,raw,endpoint,terminal[3])
    final_cards=terminal_card_evidence(stock,observed)
    evidence += [pages,raw,*final_cards]
    result={'schema':'megamanx4-independent-source-v1','source_qualification':'pass',
            'stock_source':str(stock),'observer_source':str(observed),
            'admission_tool_sha256':digest(__file__),'movie_sha256':MOVIE_SHA,'original_inputs':FRAMES,
            'neutral_tail':endpoint-FRAMES,'observed_returns':endpoint,'ram_pages':str(pages),
            'terminal_ram':str(raw),'initial_card1':str(stock/'initial-Memcard-1.bin'),
            'terminal_card1':str(final_cards[1]),'terminal_card1_sha256':digest(final_cards[1]),
            'terminal_clock':terminal[1],'terminal_ram_sha256':terminal[3].lower(),
            'images_equal':len(captured_frames(endpoint,FRAMES)),
            'bindings':[bind(p) for p in sorted(set(evidence))],
            'scope':'Completed independent source and full observer passivity; native timing/gameplay remains unqualified'}
    with output.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock',type=Path);parser.add_argument('observed',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();result=qualify(args.stock,args.observed,args.output)
    print(json.dumps({'source_reference_admitted':True,'returns':result['observed_returns'],'images_equal':result['images_equal']}))
