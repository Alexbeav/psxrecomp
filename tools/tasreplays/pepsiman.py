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

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent.parent
FRAMES=71806
MOVIE_SHA='de3fe8065a8adf068adf2cdbc2732b754df8e26aa08dc3a6f332c7dca7e5d578'
WORDS_SHA='025f465a3ff008c4bf70f9cef46ebae04c4f0c726b38a79b88bbd90e064da68d'
BIOS_SHA='9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb'
EXE_SHA='96960e14406b58dd71c6717bb374537313ff7b64215330671c989ba2926f3718'
TRACKS=[
 (124164432,'6452e78fa5d65abec2a5084afa28878bf478c9c63d6ba739b6b542cad366eded'),
 (10936800,'6cd94daf5173b9f84469683fd0270621a8ca2fbd614a6d8af1e75375c36f2531'),
 (10936800,'2255e58929497be30bded16a81e3548613b89ff7b57501219eca15e9bf557600'),
 (10936800,'8bfcd406e12e3cea8cd83f232272ef2a81581bafc0c98ffa2ed55608c6d2f8b9'),
 (10936800,'2beda20a27697e13bf84921cbca5d7363c0455ce95400ffaacd8d799ad3f7121'),
 (10936800,'bb784ddc940277585042209a07368995cc32470b9fa42e3c5245b199c5243e7d'),
 (5630688,'fcc2bbb43914bac5b3dfa79e92baaa0a94d21fc57a5d7c4b5b144643cd5a75c4d'),
 (32149488,'19c7d810996b3b56ce08ea46adcebcb6a57695b140d2d037fca409c2e409d196')]

digest=tekken3.digest
write=tekken3.write_json
command=tekken3.command

def cue_files(text):
    lines=[line.strip() for line in text.splitlines() if line.strip()]
    files=[]
    for track in range(1,9):
        if not lines: raise ValueError('missing CUE track')
        match=re.fullmatch(r'FILE "([^"\r\n]+)" BINARY',lines.pop(0))
        if not match: raise ValueError('invalid CUE file line')
        name=match[1]
        if Path(name).is_absolute() or '..' in Path(name).parts:
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

def admit_source_control(path):
    complete=json.loads((path/'complete.json').read_text())
    end=json.loads((path/'exit.json').read_text())
    semantic=json.loads((path/'semantic-review.json').read_text())
    manifest=json.loads((path/'manifest.json').read_text())
    if (complete.get('frame'),complete.get('original_inputs'),complete.get('full_movie'))!=(FRAMES,FRAMES,True):
        raise ValueError('full original source control required')
    if end.get('exit_code')!=0 or end.get('stop_reason') is not None:
        raise ValueError('source did not complete cleanly')
    if semantic.get('completion_observed') is not True or semantic.get('frame')!=FRAMES:
        raise ValueError('source completion needs semantic review')
    evidence=path/semantic['image']
    if not evidence.resolve().is_relative_to(path) or digest(evidence)!=semantic['image_sha256']:
        raise ValueError('source completion image identity changed')
    if not any(x['sha256']==MOVIE_SHA for x in manifest['bindings']):
        raise ValueError('source used a different movie')
    with (path/'ram-frames.tsv').open() as rows:
        if rows.readline().strip()!='frame\tlag_count\tpc\tram_sha256':
            raise ValueError('unrecognized stock RAM capture')
        count=0
        for expected,row in enumerate(rows):
            columns=row.rstrip('\r\n').split('\t')
            if len(columns)!=4 or int(columns[0])!=expected or not re.fullmatch('[0-9A-Fa-f]{64}',columns[3]):
                raise ValueError('incomplete or discontinuous stock RAM capture')
            count+=1
        if count!=FRAMES+1: raise ValueError('stock control misses original input boundaries')
    return {name:{'path':str(path/name),'sha256':digest(path/name)} for name in
            ['complete.json','exit.json','semantic-review.json','manifest.json','ram-frames.tsv']}

def setup(args):
    if os.name!='nt': raise ValueError('Windows UCRT build required for this candidate')
    if subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'],text=True).strip():
        raise ValueError('commit source changes before building')
    for tool in ['gcc','g++','cmake','ninja']:
        if not shutil.which(tool): raise ValueError('missing tool: '+tool)
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
    tape=project/'octoshock-cold-random.psxrng'
    command([sys.executable,HERE/'external/source_random_tape.py',tape],project/'random-tape.log')
    tekken3.require_hash(tape,tekken3.TAPE_SHA)
    payload,receipt=convert(movie.read_bytes())
    (project/'input.psxrti').write_bytes(payload)
    write(project/'input.json',receipt)
    tools=project/'tools'
    common=['-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=gcc','-DCMAKE_CXX_COMPILER=g++']
    command(['cmake','-S',ROOT/'recompiler','-B',tools,*common,'-DBUILD_TESTING=ON',
             '-DPSXRECOMP_ENABLE_CHD=ON','-DPython3_EXECUTABLE='+sys.executable],project/'configure-tools.log')
    command(['cmake','--build',tools,'--parallel',str(args.jobs)],project/'build-tools.log')
    command(['ctest','--test-dir',tools,'--output-on-failure','-j',str(args.jobs)],project/'test-tools.log')
    command([tools/'psxrecomp-toml.exe',exe,'--output',project/'census.toml','--seeds',project/'seeds.txt'],project/'census.log')
    q=lambda p:json.dumps(p.as_posix())
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
bios_config = {q(ROOT/'bios/SCPH5500.toml')}
strict = true
out_dir = {q(project/'generated')}
[runtime]
window_title = "Pepsiman - original TAS"
bios_hle = false
[video]
renderer = "software"
''',encoding='utf8')
    command([tools/'psxrecomp-bios.exe','--config',ROOT/'bios/SCPH5500.toml','--rom',bios,
             '--out-dir',ROOT/'generated'],project/'generate-bios.log')
    command([tools/'psxrecomp-game.exe','--config',game],project/'generate-game.log')
    native=project/'native'
    command(['cmake','-S',HERE,'-B',native,*common,'-DTAS_PROJECT_DIR='+str(project),
             '-DTAS_GAME_STEM=SLPS_017.62','-DTAS_EXE_NAME=Pepsiman-TAS','-DTAS_WINDOW_TITLE=Pepsiman TAS',
             '-DPSXRECOMP_BIOS_STEMS=SCPH5500','-DPSX_SHELLWIN_INTERP=ON',
             '-DPSX_RECOMP_UI=OFF','-DPSX_NETPLAY=OFF','-DPSX_REWIND=OFF','-DPSX_SETUP_WIZARD=OFF',
             '-DPSX_DEBUG_TOOLS=ON','-DPSX_ENABLE_VULKAN=OFF','-DPSXRECOMP_SKIP_BIOS_STALE_CHECK=ON',
             '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE','-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE'],project/'configure-native.log')
    command(['cmake','--build',native,'--parallel',str(args.jobs)],project/'build-native.log')
    files=[disc,bios,movie,exe,game,tape,project/'input.psxrti',project/'seeds.txt',*tracks]
    build=native/'Pepsiman-TAS.exe'
    generated={str(f.relative_to(ROOT)):digest(f) for f in (ROOT/'generated').glob('SCPH5500*') if f.is_file()}
    generated.update({str(f):digest(f) for f in (project/'generated').glob('*') if f.is_file()})
    info={'schema':'pepsiman-tas-candidate-v1','source_control':control,
          'source_head':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
          'source_tree':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD^{tree}'],text=True).strip(),
          'bindings':[{'path':str(f),'sha256':digest(f)} for f in files],
          'generated':generated,'executable':str(build),'executable_sha256':digest(build),
          'disc':str(disc),'bios':str(bios),'game':str(game),'route':str(project/'input.psxrti'),'tape':str(tape),
          'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
          'qualification':'candidate only; source/native comparison pending'}
    write(project/'setup.json',info)
    print(json.dumps({'candidate':str(build),'sha256':digest(build)}))

def run(args):
    info=json.loads((args.project/'setup.json').read_text())
    for binding in info['bindings']: tekken3.require_hash(Path(binding['path']),binding['sha256'])
    tekken3.require_hash(Path(info['executable']),info['executable_sha256'])
    argv=[sys.executable,HERE/'run_native.py',args.output,'--exe',info['executable'],
          '--game',info['game'],'--disc',info['disc'],'--bios',info['bios'],'--route',info['route'],
          '--cd-source-clock-tape',info['tape'],'--neutral-tail','1','--timeout',str(args.timeout),
          '--checkpoint-every','1200','--renderer','software',*tekken3.PROFILE]
    # The same native implementations are an explicit candidate: all psx/
    # source bytes match 2.2.2 -> 2.3, but this title still needs its own gates.
    if args.show: argv.append('--show')
    command(argv,args.output.parent/(args.output.name+'-launch.log'))
    print('Native input run ended; compare against the admitted independent source before claiming a pass.')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest='action',required=True)
    s=sub.add_parser('setup')
    for field in ['disc','bios','movie','project','cache','source-control']:
        s.add_argument('--'+field,type=Path,required=True)
    s.add_argument('--jobs',type=int,default=4)
    r=sub.add_parser('run')
    r.add_argument('--project',type=Path,required=True)
    r.add_argument('--output',type=Path,required=True)
    r.add_argument('--timeout',type=int,default=43200)
    r.add_argument('--show',action='store_true')
    args=p.parse_args()
    (setup if args.action=='setup' else run)(args)

if __name__=='__main__': main()
