"""Prepare and compare the original Pepsiman 6044 movie with owned inputs.

The native profile remains experimental until its complete source comparison
passes. Stock-source completion must precede setup. No reference is generated
from a candidate, and no movie input is changed to correct a divergence.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

import bk2_intake
from bk2_to_psxrti import convert
import tekken3
from compare_ram_pages import read_pages
from observation_evidence import compare_returns, terminal_consistency, compare_stock_observations, captured_frames

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent.parent
FRAMES=71806
MOVIE_SHA='de3fe8065a8adf068adf2cdbc2732b754df8e26aa08dc3a6f332c7dca7e5d578'
WORDS_SHA='025f465a3ff008c4bf70f9cef46ebae04c4f0c726b38a79b88bbd90e064da68d'
BIOS_SHA='9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb'
EXE_SHA='96960e14406b58dd71c6717bb374537313ff7b64215330671c989ba2926f3718'
STOCK_CORE_SHA='749d6dd58430d010e46ae97c628e113adad5f420c05c2ada0ad35e58191781c0'
STOCK_COMMIT='a15b31a46bdac27d843d3ebbc5a860012d8452fb'
OBSERVER_CORE_SHA='fce723aa1e2b73ede285863d4b1841445be47e5dd2ccb054b2e170830a7b5167'
TRACKS=[
 (124164432,'6452e78fa5d65abec2a5084afa28878bf478c9c63d6ba739b6b542cad366eded'),
 (10936800,'6cd94daf5173b9f84469683fd0270621a8ca2fbd614a6d8af1e75375c36f2531'),
 (10936800,'2255e58929497be30bded16a81e3548613b89ff7b57501219eca15e9bf557600'),
 (10936800,'8bfcd406e12e3cea8cd83f232272ef2a81581bafc0c98ffa2ed55608c6d2f8b9'),
 (10936800,'2beda20a27697e13bf84921cbca5d7363c0455ce95400ffaacd8d799ad3f7121'),
 (10936800,'bb784ddc940277585042209a07368995cc32470b9fa42e3c5245b199c5243e7d'),
 (5630688,'fcc2bbb43914bac5b3dfa79e92baa0a94d21fc57a5d7c4b5b144643cd5a75c4d'),
 (32149488,'19c7d810996b3b56ce08ea46adcebcb6a57695b140d2d037fca409c2e409d196')]

digest=tekken3.digest
write=tekken3.write_json
command=tekken3.command

def verify_control_identity(path, manifest, tail, role='stock'):
    if role not in {'stock','observer'}: raise ValueError('explicit stock/observer role required')
    if manifest.get('role',role)!=role: raise ValueError('source role differs from requested admission')
    if (manifest.get('schema'),manifest.get('source_tag'),manifest.get('original_inputs'),
        manifest.get('cutoff'),manifest.get('neutral_tail')) != ('pepsiman-stock-control-v1','2.3',FRAMES,FRAMES,tail):
        raise ValueError('source manifest does not describe the completed original movie')
    bindings=manifest['bindings']
    for binding in bindings:
        target=Path(binding['path'])
        # Legacy controls bound the host's writable config. A separately recorded
        # byte-identical copy preserves that input if EmuHawk rewrites it on exit.
        if target.name=='config.ini' and (path/'launch-config.json').exists():
            target=path/'launch-config.json'
        tekken3.require_hash(target,binding['sha256'])
    names={Path(b['path']).name:b for b in bindings}
    required={'EmuHawk.exe','octoshock.dll','start.lua','source_control.lua','source_control.py','host-closure.json'}
    if not required<=names.keys(): raise ValueError('source execution closure is incomplete')
    for entry in json.loads((path/'host-closure.json').read_text()):
        tekken3.require_hash(Path(entry['path']),entry['sha256'])
    hashes={b['sha256'] for b in bindings}
    if not {MOVIE_SHA,BIOS_SHA,*[sha for _,sha in TRACKS]}<=hashes:
        raise ValueError('source control does not bind the exact original media')
    cue=Path(next(b['path'] for b in bindings if Path(b['path']).suffix.lower()=='.cue'))
    companions=[(cue.parent/name).resolve(strict=True) for name in cue_files(cue.read_text())]
    bound={Path(b['path']).resolve():b['sha256'] for b in bindings}
    if any(bound.get(p)!=sha for p,(_,sha) in zip(companions,TRACKS)):
        raise ValueError('CUE does not resolve to the bound tracks in order')
    bios=json.loads((path/'loaded-bios.json').read_text())
    sync=json.loads((path/'effective-sync.json').read_text())
    if bios != {'sha256':BIOS_SHA,'start':'power_on','movie_length':FRAMES}:
        raise ValueError('loaded source BIOS/start identity differs')
    if sync != {'EnableLEC':False,'FIOConfig':{'Multitaps':[False,False],'Memcards':[False,False],
                                             'Devices8':[1,0,0,0,0,0,0,0]}}:
        raise ValueError('source effective controller/card configuration differs')
    if role=='observer':
        loaded=json.loads((path/'loaded-core.json').read_text())
        core=names['octoshock.dll']
        if loaded['sha256']!=core['sha256'] or Path(loaded['path']).resolve()!=Path(core['path']).resolve():
            raise ValueError('observed loaded core differs from its bound binary')
        if core['sha256']!=OBSERVER_CORE_SHA: raise ValueError('observer core is not the pinned candidate getter build')
        build=json.loads((path/'observer-build.json').read_text())
        if (build.get('exit_code'),build.get('source_head'),build.get('upstream_commit'),build.get('core_sha256')) != (
                0,manifest.get('source_commit'),STOCK_COMMIT,loaded['sha256']):
            raise ValueError('observed core build provenance differs')
        if build['source_head']!='b3ec859cc082d13b9d229dba2036fefeee96289d':
            raise ValueError('observer source must be the reviewed leaf-getter-only revision')
        if not {'Observation230.cs','Observation230.dll','observer-build.json'}<=names.keys():
            raise ValueError('observer helper source/build closure is incomplete')
    elif (names['octoshock.dll']['sha256']!=STOCK_CORE_SHA or manifest.get('source_commit')!=STOCK_COMMIT
          or (path/'loaded-core.json').exists()):
        raise ValueError('stock control must use the pinned unmodified release core')

def cue_files(text):
    lines=[line.strip() for line in text.splitlines() if line.strip()]
    files=[]
    for track in range(1,9):
        if not lines: raise ValueError('missing CUE track')
        match=re.fullmatch(r'FILE "([^"\r\n]+)" BINARY',lines.pop(0))
        if not match: raise ValueError('invalid CUE file line')
        name=match[1]
        if Path(name).anchor or ':' in name or '..' in Path(name).parts:
            raise ValueError('CUE companion must remain below the CUE directory')
        expected=[f'TRACK {track:02d} '+('MODE2/2352' if track==1 else 'AUDIO')]
        expected+=['INDEX 01 00:00:00'] if track==1 else ['INDEX 00 00:00:00','INDEX 01 00:02:00']
        if lines[:len(expected)]!=expected: raise ValueError('unqualified track order, mode or pregap')
        del lines[:len(expected)]
        files.append(name)
    if lines or len(set(files))!=8: raise ValueError('extra or repeated disc companions')
    return files

def media(cue,bios):
    tekken3.require_hash(bios,BIOS_SHA)
    tracks=[(cue.parent/name).resolve(strict=True) for name in cue_files(cue.read_text())]
    for path,(length,sha) in zip(tracks,TRACKS):
        if not path.is_relative_to(cue.parent): raise ValueError('disc companion escapes root')
        if path.stat().st_size!=length: raise ValueError('wrong track size: '+str(path))
        tekken3.require_hash(path,sha)
    return tracks

def boot_program(track):
    sys.path.insert(0,str(ROOT/'tools'))
    from prepare_disc import parse_root_entries
    with track.open('rb') as f:
        def user(lba):
            f.seek(lba*2352)
            sector=f.read(2352)
            if len(sector)!=2352 or sector[:12]!=b'\0'+b'\xff'*10+b'\0':
                raise ValueError('invalid raw data sector')
            return sector[24:2072]
        def span(lba,size):
            return b''.join(user(lba+i) for i in range((size+2047)//2048))[:size]
        pvd=user(16)
        if pvd[1:6]!=b'CD001': raise ValueError('missing ISO9660')
        tree=parse_root_entries(span(struct.unpack_from('<I',pvd,158)[0],struct.unpack_from('<I',pvd,166)[0]))
        cnf=span(*tree['SYSTEM.CNF'])
        if b'BOOT = cdrom:SLPS_017.62;1' not in cnf: raise ValueError('wrong boot program')
        data=span(*tree['SLPS_017.62'])
    if hashlib.sha256(data).hexdigest()!=EXE_SHA: raise ValueError('wrong boot executable')
    return data

def admit_source_control(path,role='stock'):
    path=path.resolve(strict=True)
    complete=json.loads((path/'complete.json').read_text())
    end=json.loads((path/'exit.json').read_text())
    semantic=json.loads((path/'semantic-review.json').read_text())
    manifest=json.loads((path/'manifest.json').read_text())
    tail=complete.get('neutral_tail')
    if type(tail) is not int or not 0<=tail<=6000:
        raise ValueError('source tail must be explicit and bounded')
    endpoint=FRAMES+tail
    if (complete.get('frame'),complete.get('original_inputs'))!=(endpoint,FRAMES) or complete.get('full_movie') is not True:
        raise ValueError('full original source control required')
    if end.get('exit_code')!=0 or end.get('stop_reason') is not None:
        raise ValueError('source did not complete cleanly')
    if semantic.get('completion_observed') is not True or type(semantic.get('frame')) is not int or not FRAMES<=semantic['frame']<=endpoint:
        raise ValueError('source completion needs semantic review')
    if semantic['image']!=f"frame-{semantic['frame']:06d}.png" or semantic['frame'] not in captured_frames(endpoint,FRAMES):
        raise ValueError('semantic evidence must name its actual captured frame')
    evidence=path/semantic['image']
    if not evidence.resolve().is_relative_to(path) or digest(evidence)!=semantic['image_sha256']:
        raise ValueError('source completion image identity changed')
    verify_control_identity(path,manifest,tail,role)
    with (path/'ram-frames.tsv').open() as rows:
        if rows.readline().strip()!='frame\tlag_count\tpc\tram_sha256':
            raise ValueError('unrecognized stock RAM capture')
        count=0
        for expected,row in enumerate(rows):
            columns=row.rstrip('\r\n').split('\t')
            if len(columns)!=4 or int(columns[0])!=expected or not re.fullmatch('[0-9A-Fa-f]{64}',columns[3]):
                raise ValueError('incomplete or discontinuous stock RAM capture')
            count+=1
        if count!=endpoint+1: raise ValueError('stock control misses declared input/neutral boundaries')
    return {name:{'path':str(path/name),'sha256':digest(path/name)} for name in
            ['complete.json','exit.json','semantic-review.json','manifest.json','ram-frames.tsv',
             'loaded-bios.json','effective-sync.json','effective-settings.json',semantic['image']]}

def setup(args):
    if os.name!='nt': raise ValueError('Windows UCRT build required for this candidate')
    if subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'],text=True).strip():
        raise ValueError('commit source changes before building')
    build_head=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip()
    if not 1<=args.jobs<=64: raise ValueError('jobs must be in 1..64')
    for tool in ['gcc','g++','cmake','ninja','git']:
        if not shutil.which(tool): raise ValueError('missing tool: '+tool)
    # The Windows System32 bash launcher is WSL, not the Git/coreutils runtime
    # used by this fingerprint script and by the Windows CMake check.
    bash=Path(shutil.which('git')).resolve().parents[1]/'bin/bash.exe'
    if not bash.is_file(): raise ValueError('Git for Windows bash is required for BIOS fingerprint verification')
    macros=subprocess.check_output(['gcc','-dM','-E','-include','_mingw.h','-'],input='',text=True)
    if not re.search(r'^#define _UCRT\b',macros,re.M): raise ValueError('UCRT compiler required')
    control=admit_source_control(args.source_control.resolve(strict=True))
    disc,bios,movie=[p.resolve(strict=True) for p in (args.disc,args.bios,args.movie)]
    tracks=media(disc,bios)
    route,intake=bk2_intake.inspect(movie.read_bytes())
    if (intake['movie_sha256'],intake['frame_count'],intake['pad_words_le_sha256'])!=(MOVIE_SHA,FRAMES,WORDS_SHA):
        raise ValueError('wrong Pepsiman movie')
    project=args.project.resolve()
    if project==ROOT or project.is_relative_to(ROOT): raise ValueError('put generated title/build output outside source')
    project.mkdir(parents=True,exist_ok=False)
    data=boot_program(tracks[0])
    cache=args.cache.resolve()/EXE_SHA
    cache.mkdir(parents=True,exist_ok=True)
    exe=cache/'SLPS_017.62'
    if exe.exists(): tekken3.require_hash(exe,EXE_SHA)
    else: exe.write_bytes(data)
    # WinLibs std::filesystem::absolute rebases a valid UNC path onto the
    # current drive. Use the same verified private content cache as the boot
    # executable; record both original and staged firmware identities.
    bios_cache=args.cache.resolve()/BIOS_SHA
    bios_cache.mkdir(parents=True,exist_ok=True)
    staged_bios=bios_cache/'SCPH5500.BIN'
    if staged_bios.exists(): tekken3.require_hash(staged_bios,BIOS_SHA)
    else:
        shutil.copyfile(bios,staged_bios)
        tekken3.require_hash(staged_bios,BIOS_SHA)
    tape=project/'octoshock-cold-random.psxrng'
    command([sys.executable,HERE/'external/source_random_tape.py',tape],project/'random-tape.log')
    tekken3.require_hash(tape,tekken3.TAPE_SHA)
    payload,receipt=convert(movie.read_bytes())
    (project/'input.psxrti').write_bytes(payload)
    write(project/'input.json',receipt)
    tools=args.tools_dir.resolve() if args.tools_dir else project/'tools'
    common=['-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=gcc','-DCMAKE_CXX_COMPILER=g++']
    command(['cmake','-S',ROOT/'recompiler','-B',tools,*common,'-DBUILD_TESTING=ON',
             '-DPSXRECOMP_ENABLE_CHD=ON','-DPython3_EXECUTABLE='+sys.executable],project/'configure-tools.log')
    command(['cmake','--build',tools,'--parallel',str(args.jobs)],project/'build-tools.log')
    command(['ctest','--test-dir',tools,'--output-on-failure','-j',str(args.jobs)],project/'test-tools.log')
    command([tools/'psxrecomp-toml.exe',exe,'--output',project/'census.toml','--seeds',project/'seeds.txt'],project/'census.log')
    q=lambda p:json.dumps(p.as_posix())
    bios_profile=project/'bios.toml'
    profile=(ROOT/'bios/SCPH5500.toml').read_text()
    profile=re.sub(r'^rom\s*=.*$',lambda _: 'rom = '+q(staged_bios),profile,flags=re.M)
    profile=re.sub(r'^seeds\s*=.*$',lambda _: 'seeds = '+q(ROOT/'recompiler/seeds/phase2_ghidra_seeds_SCPH5500.json'),profile,flags=re.M)
    profile=re.sub(r'^out_dir\s*=.*$',lambda _: 'out_dir = '+q(ROOT/'generated'),profile,flags=re.M)
    bios_profile.write_text(profile,encoding='utf8')
    game=project/'game.toml'
    game.write_text(f'''[game]
name = "Pepsiman TAS"
id = "SLPS-01762"
exe = {q(exe)}
load_address = "0x80010000"
entry_pc = "0x80042C58"
text_size = "0x85800"
stack_base = "0x801FFFF0"
[recompiler]
seeds = {q(project/'seeds.txt')}
bios_config = {q(bios_profile)}
strict = true
out_dir = {q(project/'generated')}
[runtime]
window_title = "Pepsiman - original TAS"
bios_hle = false
[video]
renderer = "software"
''',encoding='utf8')
    command([tools/'psxrecomp-bios.exe','--config',bios_profile,'--rom',staged_bios,
             '--out-dir',ROOT/'generated'],project/'generate-bios.log')
    fingerprint=subprocess.check_output([str(bash),(ROOT/'tools/bios_emitter_fingerprint.sh').as_posix(),
                                         bios_profile.as_posix()],cwd=ROOT,text=True).strip()
    if not re.fullmatch('[0-9a-f]{64}',fingerprint): raise ValueError('invalid generated BIOS fingerprint')
    (ROOT/'generated/SCPH5500.emitter.sha').write_text(fingerprint+'\n')
    command([tools/'psxrecomp-game.exe','--config',game],project/'generate-game.log')
    native=project/'native'
    command(['cmake','-S',HERE,'-B',native,*common,'-DTAS_PROJECT_DIR='+str(project),
             '-DTAS_GAME_STEM=SLPS_017.62','-DTAS_EXE_NAME=Pepsiman-TAS','-DTAS_WINDOW_TITLE=Pepsiman TAS',
             '-DPSXRECOMP_BIOS_STEMS=SCPH5500','-DPSX_SHELLWIN_INTERP=ON',
             '-DPSXRECOMP_BIOS_PROFILE='+str(bios_profile),
             '-D_psxrt_bash='+str(bash),
             '-DPSX_RECOMP_UI=OFF','-DPSX_NETPLAY=OFF','-DPSX_REWIND=OFF','-DPSX_SETUP_WIZARD=OFF',
             '-DPSX_DEBUG_TOOLS=ON','-DPSX_ENABLE_VULKAN=OFF',
             '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE','-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE'],project/'configure-native.log')
    command(['cmake','--build',native,'--parallel',str(args.jobs)],project/'build-native.log')
    if (subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'],text=True).strip() or
        subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip()!=build_head):
        raise ValueError('source changed during candidate build')
    files=[disc,bios,staged_bios,bios_profile,movie,exe,game,tape,project/'input.psxrti',project/'seeds.txt',*tracks]
    build=native/'Pepsiman-TAS.exe'
    generated={str(f.relative_to(ROOT)):digest(f) for f in (ROOT/'generated').glob('SCPH5500*') if f.is_file()}
    generated.update({str(f):digest(f) for f in (project/'generated').glob('*') if f.is_file()})
    info={'schema':'pepsiman-tas-candidate-v1','source_control':control,
          'source_head':build_head,
          'source_tree':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD^{tree}'],text=True).strip(),
          'bindings':[{'path':str(f),'sha256':digest(f)} for f in files],
          'generated':generated,'executable':str(build),'executable_sha256':digest(build),
          'disc':str(disc),'bios':str(bios),'game':str(game),'route':str(project/'input.psxrti'),'tape':str(tape),
          'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
          'tools_build_dir':str(tools),
          'qualification':'candidate only; source/native comparison pending'}
    write(project/'setup.json',info)
    print(json.dumps({'candidate':str(build),'sha256':digest(build)}))

def run(args):
    info=json.loads((args.project/'setup.json').read_text())
    for binding in info['bindings']: tekken3.require_hash(Path(binding['path']),binding['sha256'])
    tekken3.require_hash(Path(info['executable']),info['executable_sha256'])
    reference=None
    observation_end=FRAMES
    if args.reference:
        reference=json.loads(args.reference.read_text())
        if reference.get('schema')!='pepsiman-independent-source-v1' or reference.get('movie_sha256')!=MOVIE_SHA:
            raise ValueError('wrong independent source reference')
        for binding in reference['bindings']:
            tekken3.require_hash(Path(binding['path']),binding['sha256'])
        observation_end=reference['observed_returns']
        if type(observation_end) is not int or not FRAMES<=observation_end<=FRAMES+6000:
            raise ValueError('invalid reference boundary')
    argv=[sys.executable,HERE/'run_native.py',args.output,'--exe',info['executable'],
          '--game',info['game'],'--disc',info['disc'],'--bios',info['bios'],'--route',info['route'],
          '--cd-source-clock-tape',info['tape'],'--neutral-tail',str(observation_end-FRAMES+1),'--timeout',str(args.timeout),
          '--storage-budget-mib','1536',
          '--checkpoint-every','1200','--renderer','software',*tekken3.PROFILE]
    # The same native implementations are an explicit candidate: all psx/
    # source bytes match 2.2.2 -> 2.3, but this title still needs its own gates.
    if args.show: argv.append('--show')
    if reference: argv += ['--ram-snapshot-frame',str(observation_end)]
    args.output.parent.mkdir(parents=True,exist_ok=True)
    if args.output.exists(): raise ValueError('choose a fresh native output directory')
    log=args.output.parent/(args.output.name+'-launch.log')
    with log.open('x') as stream:
        process=subprocess.run([str(v) for v in argv],cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT)
    report={'status':'incomplete','original_inputs':FRAMES,'compared_returns':0,
            'end_frame':observation_end+1,'source_observation_end':observation_end,
            'native_runner_exit':process.returncode,'first_divergence':None,
            'scope':'unchanged original inputs and declared neutral ending; full RAM/clock source compatibility'}
    if reference:
        try:
            comparison=compare_returns(Path(reference['ram_pages']),args.output/'ram-pages.tsv',observation_end)
            report.update({k:v for k,v in comparison.items() if k!='match'})
            complete=json.loads((args.output/'complete.json').read_text())
            report['input_identity_matches']=(complete['input_frames']==FRAMES and complete['applied_words_sha256']==WORDS_SHA
                and complete['frame']==observation_end+1 and complete['neutral_tail_ticks']==observation_end-FRAMES+1)
            raw=terminal_consistency(Path(reference['ram_pages']),Path(reference['terminal_ram']),observation_end)
            actual=terminal_consistency(args.output/'ram-pages.tsv',args.output/f'ram-frame-{observation_end:06d}.bin',observation_end)
            report['terminal_ram_matches']=actual==raw
            okay=process.returncode==0 and report['input_identity_matches'] and report['terminal_ram_matches']
            okay=okay and comparison['match']
            report['status']='pass' if okay else 'fail'
        except (ValueError,OSError,KeyError) as error:
            report.update(status='fail',error=str(error))
    elif process.returncode:
        report['status']='fail'
    if (args.output/'exit.json').exists():
        host=json.loads((args.output/'exit.json').read_text())
        if host.get('timed_out') or host.get('stop_reason') in {'host_timeout','host_storage_budget'}:
            report.update(status='incomplete',host_limit=host.get('stop_reason') or 'host_timeout',
                          classification='host resource limit; no guest failure inferred')
    write(args.output/'verification.json',report)
    print(json.dumps(report))
    return 0 if report['status']=='pass' else 2 if report['status']=='incomplete' else 1

def reference(args):
    stock=args.stock.resolve(strict=True)
    observed=args.observed.resolve(strict=True)
    stock_identity=admit_source_control(stock)
    observed_identity=admit_source_control(observed,'observer')
    if (stock/'loaded-core.json').exists() or not (observed/'loaded-core.json').exists():
        raise ValueError('reference requires distinct stock and rebuilt observer roles')
    endpoint=json.loads((observed/'complete.json').read_text())['frame']
    if json.loads((stock/'complete.json').read_text())['frame']!=endpoint:
        raise ValueError('stock and observer endpoints differ')
    compared_files=compare_stock_observations(stock,observed,endpoint)
    raw=observed/f'ram-frame-{endpoint:06d}.bin'
    with (observed/'ram-frames.tsv').open() as stream:
        for line in stream: terminal_line=line
    terminal_consistency(observed/'ram-pages.tsv',raw,endpoint,terminal_line.rstrip().split('\t')[3])
    if args.output.exists(): raise ValueError('never replace an admitted reference')
    files=[observed/'ram-pages.tsv',raw,observed/'loaded-core.json',observed/'stock-comparison.json',
           observed/'semantic-review.json',stock/'semantic-review.json',stock/'ram-frames.tsv',
           observed/'manifest.json',stock/'manifest.json',*compared_files]
    for directory in [stock,observed]:
        manifest=json.loads((directory/'manifest.json').read_text())
        files += [Path(b['path']) if Path(b['path']).name!='config.ini' else directory/'launch-config.json'
                  for b in manifest['bindings']]
        files += [Path(b['path']) for b in json.loads((directory/'host-closure.json').read_text())]
        files += [Path(v['path']) for v in (stock_identity if directory==stock else observed_identity).values()]
    files=sorted(set(files))
    result={'schema':'pepsiman-independent-source-v1','movie_sha256':MOVIE_SHA,'bios_sha256':BIOS_SHA,
            'observed_returns':endpoint,'original_inputs':FRAMES,'neutral_tail':endpoint-FRAMES,
            'ram_pages':str(observed/'ram-pages.tsv'),'terminal_ram':str(raw),
            'stock_control':stock_identity,'observed_control':observed_identity,
            'bindings':[{'path':str(f),'sha256':digest(f)} for f in files],
            'scope':'qualified Octoshock 2.3 observer and witnessed source endpoint; not whole-machine hardware accuracy'}
    write(args.output,result)
    print(args.output)
    return 0

def main():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest='action',required=True)
    s=sub.add_parser('setup')
    for field in ['disc','bios','movie','project','cache','source-control']:
        s.add_argument('--'+field,type=Path,required=True)
    s.add_argument('--jobs',type=int,default=4)
    s.add_argument('--tools-dir',type=Path,help='Reuse a CMake tool build for this source; reconfigure, rebuild and rerun all tests')
    r=sub.add_parser('run')
    r.add_argument('--project',type=Path,required=True)
    r.add_argument('--output',type=Path,required=True)
    r.add_argument('--timeout',type=int,default=43200)
    r.add_argument('--show',action='store_true')
    r.add_argument('--reference',type=Path)
    ref=sub.add_parser('reference')
    for field in ['stock','observed','output']: ref.add_argument('--'+field,type=Path,required=True)
    args=p.parse_args()
    return {'setup':setup,'run':run,'reference':reference}[args.action](args)

if __name__=='__main__': raise SystemExit(main())
