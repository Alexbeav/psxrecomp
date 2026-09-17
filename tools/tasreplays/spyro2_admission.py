"""Admit the independently replayed Spyro 2 Nymashock source, never native output.

Publication 5278M, "all Moneybags unlocks", a native BizHawk 2.8 Nymashock recording, replayed
on the pinned 2.8 host through source_control_nyma28.py. Structure and every check are
medievil_admission.py's; a copy rather than a second entry there, because that module's bytes are
hashed into the admitted MediEvil reference.

Two differences, both forced by this movie:
- It declares the port-1 memory card OFF (SyncSettings MednafenValues psx.input.port1.memcard "0"),
  so there is no initial card, no terminal card and no card comparison. initial-cards.json is [].
- Its helper is observer-280-cards, the Observation291 variant that checks the effective card
  settings against the movie's own SyncSettings instead of the fixed port-1-on layout.
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

FRAMES=86655
SOURCE_COMMIT='e731e0f32903cd40b83ed75bba3b1e3753105ce2'
MOVIE_SHA='d434e6efaa53983f60ea21247b11c6016901419ecb35f677f5af882cff76b0aa'
TITLE='Spyro 2 Riptos Rage (USA) all Moneybags 5278M'
FIRMWARE_KEY='PSX+U'
# The firmware this movie's own header declares. Its header says
# PSX_Firmware_U 0555C6FAE8906F3F09BAF5988F00E55F88E9F30B, which is SCPH-5501;
# admission cross-checks that SHA-1 against the loaded BIOS below, so this pin
# and the movie can never drift apart again.
FIRMWARE_NAME='SCPH5501.BIN'
FIRMWARE_SHA1='0555c6fae8906f3f09baf5988f00e55f88e9f30b'
FIXED={
    "Spyro 2 - Ripto's Rage! (USA).bin":'b3fd9fdb24b115fee4583dbe3c36ab788fd8679fb4592dbec8736392dec64551',
    "Spyro 2 - Ripto's Rage! (USA).cue":'b1f83629212064f23e959d090abff04080b434b79aa85735642d8267aca8c92c',
    'BizHawk-2.8-win-x64.zip':'cbbbeb86684ad504ae818dfe5a151174e4a6f087afa5022e4775e05798a1e060',
    'EmuHawk.exe':'42ef3c25d6b215c6b19dd9ae7924ea20e53f800a736ead91081d12e204a918f5',
    'BizHawk.Emulation.Cores.dll':'c56c12aa2d03c13274995738a01f34a4883ee98d6018ed5ad5fa9695cf740d54',
    'shock.wbx.gz':'c2169a211b19c2ec890a4ddbebf6e839b7c22615f2af17d941ea9ca8a80010ba',
    FIRMWARE_NAME:'11052b6499e466bbf0a709b1f9cb6834a9418e66680387912451e971cf8a1fef',
    'lapogne36-spyro2-moneybags.bk2':MOVIE_SHA,'original.bk2':MOVIE_SHA,
    'Observation291.dll':'13d2a18d2fb1108d7b982ecf35972293192edf341c3f1e4dd89c43a20fc3bf36',
    'source_control_nyma28.py':'01048acdb17e3f1c7c2cd0bdeaf503ccb4a8e80db23b5de574bc8f9d397e665f',
    'source_control_nyma28.lua':'90738adc9ff79b6b312dd5d243fa98bd0e69ab4422f825c615509aa55fa1eca1',
    'observer-build.json':'0643389b895c12211b0b3135feefbcfe947b93b49b07eeb6902772e221b989a4',
}
# Run-local artifacts the launcher writes or copies beside the run's manifest.
LOCAL=('original.bk2','source_control_nyma28.py','source_control_nyma28.lua','observer-build.json','start.lua','launch-config.json','host-closure.json')
# Unbound diagnostic evidence: controller.tsv is the host-side joypad.get() log (neutral during
# playback; on 2.8 the triangle, circle and square keys are matched by their low byte) and
# host-dialogs.jsonl records any dismissed informational host dialog.
OPTIONAL=('controller.tsv','host-dialogs.jsonl')
# Canonical JSON identities from the qualified source prefix's resolved defaults.
SETTINGS={
    'effective-sync.json':'2ab1cc6f3085acf21fbf54b944b29970c1f4a808b9265a2dc7d90f5a55c475ad',
    'effective-settings.json':'9f78b1634dac17e574889add799f181c3f37f7ac8508bf92e5b9cae964652ad3',
    'effective-setting-values.json':'66bd1705ab3c7a14dc297a91188a57e45c0803e3f086d163cfc8eb70393d4bc6',
    'effective-ports.json':'e3ceec7164b05978e42d0ad200027a700f4a0ec607f1630ea2b800d72444168c',
}

def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def read(path):return json.loads(Path(path).read_text())

def bind(path):return {'path':str(Path(path).resolve()),'sha256':digest(path)}

# This title runs with no memory card, so there are no card helpers: the movie declares
# port-1 memcard "0", initial-cards.json is empty and nothing persists a card image.

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
    return {'LastWrittenFrom':'2.8','LastWrittenFromDetailed':'Version 2.8','FirmwareUserSpecifications':{FIRMWARE_KEY:firmware},
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
        'nymashock-stock-control-v2',TITLE,SOURCE_COMMIT,'2.8',FRAMES,FRAMES,role,tail,FIRMWARE_KEY,FIXED[FIRMWARE_NAME]):
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
    with zipfile.ZipFile(names['BizHawk-2.8-win-x64.zip']['path']) as archive:
        for item in closure:
            path=Path(item['path']).resolve()
            if path.relative_to(app.parent).as_posix()!=item['archive_member']:
                raise ValueError('host closure archive member differs')
            if digest(path)!=item['sha256'] or hashlib.sha256(archive.read(item['archive_member'])).hexdigest()!=item['sha256']:
                raise ValueError('changed stock host closure')
    actual={p.resolve() for p in app.parent.rglob('*') if p.is_file() and
            (p.suffix.lower() in {'.exe','.dll','.wbx','.config'} or p.name.lower().endswith('.wbx.gz'))}
    if actual!={Path(x['path']).resolve() for x in closure}:raise ValueError('incomplete host artifact closure')
    expected_lua=('CONTROL_ROOT=[['+root.as_posix()+']]\nCONTROL_LENGTH='+str(FRAMES)+'\nCONTROL_END='+str(FRAMES)+'\nCONTROL_TAIL='+str(tail)+
        '\nCONTROL_HELPER=[['+Path(names['Observation291.dll']['path']).as_posix()+']]\nCONTROL_CORE_SHA="'+
        FIXED['BizHawk.Emulation.Cores.dll']+'"\nCONTROL_WBX_SHA="'+FIXED['shock.wbx.gz']+
        '"\nCONTROL_WITH_PAGES='+str(role=='pages-observer').lower()+'\nCONTROL_BIOS_SHA="'+FIXED[FIRMWARE_NAME]+'"\n'+
        (root/'source_control_nyma28.lua').read_text())
    if (root/'start.lua').read_text()!=expected_lua:raise ValueError('source launch script differs')
    if read(root/'launch-config.json')!=launch_config(root,names[FIRMWARE_NAME]['path']):raise ValueError('source launch configuration differs')
    expected_command=[str(app),str(Path(names["Spyro 2 - Ripto's Rage! (USA).cue"]['path'])),
        '--config='+str(root/'host-config.json'),'--movie='+str(root/'original.bk2'),'--lua='+str(root/'start.lua')]
    if manifest['command']!=expected_command:raise ValueError('source command differs')
    loaded=read(root/'loaded-core.json')
    if (loaded.get('type'),loaded.get('clock'),loaded.get('assembly_sha256'),loaded.get('waterbox_sha256')) != (
        'BizHawk.Emulation.Cores.Sony.PSX.Nymashock','public WaterboxCore.CycleCount',FIXED['BizHawk.Emulation.Cores.dll'],FIXED['shock.wbx.gz']):
        raise ValueError('loaded Nymashock core differs')
    for field,name in [('assembly','BizHawk.Emulation.Cores.dll'),('waterbox','shock.wbx.gz')]:
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
    if read(root/'initial-cards.json')!=[]:
        raise ValueError('source must attach no memory card')
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
    evidence=[root/name for name in ['complete.json','exit.json','manifest.json','semantic-review.json','ram-frames.tsv','loaded-core.json','loaded-bios.json','initial-cards.json','effective-run-policy.json',*SETTINGS,review['image']]]
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
    for name in ['loaded-core.json','initial-cards.json','effective-ports.json','effective-setting-values.json']:
        if (stock/name).read_bytes()!=(observed/name).read_bytes():raise ValueError('source observer identity differs: '+name)
    pages=observed/'ram-pages.tsv';raw=observed/f'ram-frame-{endpoint:06d}.bin'
    rows=return_rows(stock/'ram-frames.tsv',endpoint);next(rows)
    for a,b in zip_longest(rows,read_pages(pages)):
        if a is None or b is None or a[:2]!=b[:2]:raise ValueError('source page clocks/coverage differ')
    terminal_consistency(pages,raw,endpoint,terminal[3])
    evidence += [pages,raw]
    result={'schema':'spyro2-independent-source-v1','source_qualification':'pass',
            'stock_source':str(stock),'observer_source':str(observed),
            'admission_tool_sha256':digest(__file__),'movie_sha256':MOVIE_SHA,'original_inputs':FRAMES,
            'neutral_tail':endpoint-FRAMES,'observed_returns':endpoint,'ram_pages':str(pages),
            'terminal_ram':str(raw),'memory_card':None,
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
