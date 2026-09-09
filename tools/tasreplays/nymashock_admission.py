"""Admit the independently replayed Bio Hazard Nymashock source, never native output."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import zipfile
from itertools import zip_longest
from observation_evidence import captured_frames, compare_stock_observations, terminal_consistency
from compare_ram_pages import read_pages

FRAMES=227202
SOURCE_COMMIT='745efb1dd8eb82f31ba9201a79cdfc5bcaf1f5d1'
MOVIE_SHA='821e4aff88eadab5c7b175b70c9ccfbfee65d59eec27256d4ce86cd7c83e88aa'
CARD_SHA='78b6d4ac9ab4d23caf7e5f04f83539bf5d994cccfb0a709d14ac53d05c8e21ef'
FIXED={
    "Bio Hazard - Director's Cut (Japan).bin":'007dc49b5899ff0aa55b6df7caea082c89aa137ff32d29ed36f28546e28e417e',
    "Bio Hazard - Director's Cut (Japan).cue":'0acbf59f7d28e632c7d8f55de72380b028c00c14ad88deb940a0d9aac6afe515',
    'BizHawk-2.9.1-win-x64.zip':'ff6ddc657d474e37d3b06cb10154f19173a1455ab756476ed58236fca62387a9',
    'EmuHawk.exe':'6ce622d4ed4e8460ce362cf35ef67dc70096fec2c9a174cbef6a3e5b04f18bcc',
    'BizHawk.Emulation.Cores.dll':'750b0dfb9a3b9720ae92ed94aa798d7b531f14c20d202a97be6c83568e620bbe',
    'shock.wbx.zst':'78c7bdda5ad9551294468fb435eab7afb7e8edb224ef44c7bf7efaa4032dd200',
    'PSX - SCPH5500.BIN':'9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb',
    're1-submission8957.bk2':MOVIE_SHA,'re1-original.bk2':MOVIE_SHA,
    'Observation291.dll':'49ebfc6afc1e4f1a49b498845010a8a849fde674cfeae242f82620c1ef285ebc',
    'source_control291.py':'6a349dc476a74864ef204786cffb55b0fc9c0d9d177eeec73dc18bcddf715579',
    'source_control291.lua':'460d2d51ffe644e0f56244d6086c9e531eadd63e0ac4c3f358d5c53f6e994965',
    'observer-build.json':'74543a13eb85a4cf9ec33db84e83848a20817f25ad59ce64913f0e5a6945198e',
}
# Canonical JSON identities from the qualified source prefix's resolved defaults.
SETTINGS={
    'effective-sync.json':'066f68ba9ee8aa8be5753dd8b69056bf479b2fea00e4c22f5072e58621e96f97',
    'effective-settings.json':'9f78b1634dac17e574889add799f181c3f37f7ac8508bf92e5b9cae964652ad3',
    'effective-setting-values.json':'4683a464ab25e69738314759f6530627813b7507480edcdea0cb01c3004daff4',
    'effective-ports.json':'e3ceec7164b05978e42d0ad200027a700f4a0ec607f1630ea2b800d72444168c',
}

def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def read(path):return json.loads(Path(path).read_text())

def bind(path):return {'path':str(Path(path).resolve()),'sha256':digest(path)}

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

def verify_identity(root,manifest,role,tail):
    if (manifest.get('schema'),manifest.get('source_commit'),manifest.get('source_tag'),
        manifest.get('original_inputs'),manifest.get('cutoff'),manifest.get('role'),manifest.get('neutral_tail')) != (
        're1-stock-control-v1',SOURCE_COMMIT,'2.9.1',FRAMES,FRAMES,role,tail):
        raise ValueError('source manifest role/core/input boundary differs')
    bindings=manifest['bindings'];names={Path(x['path']).name:x for x in bindings}
    if len(names)!=len(bindings) or set(names)!=set(FIXED)|{'start.lua','launch-config.json','host-closure.json'}:
        raise ValueError('source binding closure differs')
    for name,item in names.items():
        if item['sha256']!=FIXED.get(name,item['sha256']) or digest(item['path'])!=item['sha256']:
            raise ValueError('changed source artifact: '+name)
        if name in ('re1-original.bk2','source_control291.py','source_control291.lua','observer-build.json','start.lua','launch-config.json','host-closure.json'):
            if Path(item['path']).resolve()!=root/name:raise ValueError('run-local source artifact escapes: '+name)
    closure=read(root/'host-closure.json')
    if not closure or len({x['path'] for x in closure})!=len(closure):raise ValueError('invalid host closure')
    app=Path(names['EmuHawk.exe']['path']).resolve()
    with zipfile.ZipFile(names['BizHawk-2.9.1-win-x64.zip']['path']) as archive:
        for item in closure:
            path=Path(item['path']).resolve()
            if path.relative_to(app.parent).as_posix()!=item['archive_member']:
                raise ValueError('host closure archive member differs')
            if digest(path)!=item['sha256'] or hashlib.sha256(archive.read(item['archive_member'])).hexdigest()!=item['sha256']:
                raise ValueError('changed stock host closure')
    actual={p.resolve() for p in app.parent.rglob('*') if p.is_file() and
            (p.suffix.lower() in {'.exe','.dll','.wbx','.config'} or p.name.lower().endswith('.wbx.zst'))}
    if actual!={Path(x['path']).resolve() for x in closure}:raise ValueError('incomplete host artifact closure')
    expected_lua=('CONTROL_ROOT=[['+root.as_posix()+']]\nCONTROL_END='+str(FRAMES)+'\nCONTROL_TAIL='+str(tail)+
        '\nCONTROL_HELPER=[['+Path(names['Observation291.dll']['path']).as_posix()+']]\nCONTROL_CORE_SHA="'+
        FIXED['BizHawk.Emulation.Cores.dll']+'"\nCONTROL_WBX_SHA="'+FIXED['shock.wbx.zst']+
        '"\nCONTROL_WITH_PAGES='+str(role=='pages-observer').lower()+'\n'+(root/'source_control291.lua').read_text())
    if (root/'start.lua').read_text()!=expected_lua:raise ValueError('source launch script differs')
    expected_command=[str(app),str(Path(names["Bio Hazard - Director's Cut (Japan).cue"]['path'])),
        '--config='+str(root/'host-config.json'),'--movie='+str(root/'re1-original.bk2'),'--lua='+str(root/'start.lua')]
    if manifest['command']!=expected_command:raise ValueError('source command differs')
    loaded=read(root/'loaded-core.json')
    if (loaded.get('type'),loaded.get('clock'),loaded.get('assembly_sha256'),loaded.get('waterbox_sha256')) != (
        'BizHawk.Emulation.Cores.Sony.PSX.Nymashock','public WaterboxCore.CycleCount',FIXED['BizHawk.Emulation.Cores.dll'],FIXED['shock.wbx.zst']):
        raise ValueError('loaded Nymashock core differs')
    for field,name in [('assembly','BizHawk.Emulation.Cores.dll'),('waterbox','shock.wbx.zst')]:
        if Path(loaded[field]).resolve()!=Path(names[name]['path']).resolve():raise ValueError('loaded core path differs')
    if read(root/'loaded-bios.json')!={'sha256':FIXED['PSX - SCPH5500.BIN'],'bytes':524288}:
        raise ValueError('loaded BIOS differs')
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
    evidence += [pages,raw]
    result={'schema':'biohazard-independent-source-v1','movie_sha256':MOVIE_SHA,'original_inputs':FRAMES,
            'neutral_tail':endpoint-FRAMES,'observed_returns':endpoint,'ram_pages':str(pages),
            'terminal_ram':str(raw),'initial_card1':str(stock/'initial-Memcard-1.bin'),
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
