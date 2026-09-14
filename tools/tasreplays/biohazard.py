"""Build the exact Bio Hazard Director's Cut candidate after source admission.

Runtime models below are explicit comparison candidates. Their older profile
names do not establish Nymashock equivalence; source RAM/clock checks do that.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,struct,subprocess,sys
import build_cache
import dualshock_route
import nymashock_admission as source
from observation_evidence import terminal_consistency
from run_native import route_identity
from tekken3 import command,require_hash,write_json
from replay_prefix import native_span,write_prefix_route,parse_ladder,run_ladder
from launch_identity import resolve_binary,receipt_fields,add_launch_arguments,check_launch_arguments,run_native_arguments
from stream_compare import Watcher,page_reference

HERE=Path(__file__).resolve().parent;ROOT=HERE.parent.parent
BOOT='SLPS_009.98'
EXE_SHA='44850e2aa72edebdb986141b49874a08160fbb103bffa505ceba9ba0a19bd0b2'
CONTROLLER_SHA='09d4015e5801e62311334b22f38e766cfe0b46ba38fe45f2caf5f3fd0a5c6baa'
TAPE_SHA='d85f0dec13b00e50b10a52ce0fda3cdaa9797f058ad16dd57caa8e926fad7a6a'
PROFILE=[
    '--critical-section-model','exception','--syscall-model','guest-exception',
    '--field-model','octoshock-2.2.2-ntsc-raster',
    '--dma-model','octoshock-2.2.2-otc',
    '--pad-ack-model','nymashock-1.29.0-dualshock',
    '--card-model','nymashock-1.29.0','--legacy-card-repair','off',
    '--cd-firmware-model','octoshock-2.2.2','--cd-cold-status-model','octoshock-2.2.2',
    '--cd-toc-seek-model','octoshock-2.2.2','--cd-explicit-seek-model','octoshock-2.2.2',
    '--cd-read-start-model','octoshock-2.2.2-pipeline','--cd-dma-model','octoshock-2.2.2',
    '--cd-drive-model','nymashock-1.29.0',
    '--cd-cdda-model','octoshock-2.3','--mdec-source-model','nymashock-1.29.0',
    '--gpu-status-model','octoshock-2.2.2-raster','--gpu-dma-model','octoshock-2.2.2-bounded-quad',
    '--timer1-model','octoshock-2.2.2','--timer2-model','octoshock-2.2.2',
    '--precise-slice','on','--cpu-return-probe','--ram-page-probe',
]

def verify_reference(path):
    ref=source.read(path)
    if ref.get('schema')!='biohazard-independent-source-v1' or ref.get('source_qualification')!='pass' or ref.get('movie_sha256')!=source.MOVIE_SHA or ref.get('original_inputs')!=source.FRAMES:
        raise ValueError('wrong independent Bio Hazard source reference')
    endpoint=ref.get('observed_returns')
    if type(endpoint) is not int or not source.FRAMES<=endpoint<=source.FRAMES+12000 or ref.get('neutral_tail')!=endpoint-source.FRAMES:
        raise ValueError('invalid source reference endpoint')
    bindings={str(Path(x['path']).resolve()):x['sha256'] for x in ref['bindings']}
    if len(bindings)!=len(ref['bindings']):raise ValueError('duplicate source reference binding')
    for p,h in bindings.items():require_hash(Path(p),h)
    for key in ('ram_pages','terminal_ram','initial_card1','terminal_card1'):
        if str(Path(ref[key]).resolve()) not in bindings:raise ValueError('unbound source reference field')
    if ref.get('admission_tool_sha256')!=source.digest(Path(source.__file__)):
        raise ValueError('source reference admission tool differs')
    stock,observed=Path(ref['stock_source']).resolve(),Path(ref['observer_source']).resolve()
    if stock==observed:raise ValueError('source reference roles must be independent')
    if (Path(ref['ram_pages']).resolve()!=observed/'ram-pages.tsv' or
        Path(ref['terminal_ram']).resolve()!=observed/f'ram-frame-{endpoint:06d}.bin' or
        Path(ref['initial_card1']).resolve()!=stock/'initial-Memcard-1.bin' or
        Path(ref['terminal_card1']).resolve()!=observed/source.FINAL_CARD):
        raise ValueError('source reference fields do not belong to the admitted roles')
    for root,role in ((stock,'stock-control'),(observed,'pages-observer')):
        for name in ('manifest.json','complete.json','exit.json','semantic-review.json'):
            if str(root/name) not in bindings:raise ValueError('source reference is missing admission evidence')
        admitted,terminal,_=source.admit(root,role)
        if admitted!=endpoint or terminal[1]!=ref['terminal_clock'] or terminal[3].lower()!=ref['terminal_ram_sha256']:
            raise ValueError('source reference terminal differs')
    require_hash(Path(ref['initial_card1']),source.CARD_SHA)
    require_hash(Path(ref['terminal_card1']),ref['terminal_card1_sha256'])
    source.terminal_card_evidence(stock,observed)
    return ref

def media(cue,bios):
    require_hash(cue,source.FIXED["Bio Hazard - Director's Cut (Japan).cue"])
    require_hash(bios,source.FIXED['PSX - SCPH5500.BIN'])
    lines=[x.strip() for x in cue.read_text().splitlines() if x.strip()]
    match=re.fullmatch(r'FILE "([^"\r\n]+)" BINARY',lines[0]) if lines else None
    if not match or lines[1:]!=['TRACK 01 MODE2/2352','INDEX 01 00:00:00']:
        raise ValueError('wrong single-track Japanese Director\'s Cut topology')
    track=(cue.parent/match[1]).resolve(strict=True)
    if not track.is_relative_to(cue.parent.resolve()) or track.stat().st_size!=719540304:
        raise ValueError('invalid Bio Hazard data track')
    require_hash(track,source.FIXED["Bio Hazard - Director's Cut (Japan).bin"])
    return track

def boot_program(track):
    sys.path.insert(0,str(ROOT/'tools'))
    from prepare_disc import parse_root_entries
    with track.open('rb') as f:
        def user(lba):
            f.seek(lba*2352);sector=f.read(2352)
            if len(sector)!=2352 or sector[:12]!=b'\0'+b'\xff'*10+b'\0' or sector[15]!=2:
                raise ValueError('invalid raw Mode2 data sector')
            return sector[24:2072]
        def span(lba,size):
            if not 0<size<=2097152:raise ValueError('boot metadata outside bounded size')
            return b''.join(user(lba+i) for i in range((size+2047)//2048))[:size]
        pvd=user(16)
        if pvd[1:6]!=b'CD001':raise ValueError('missing ISO9660')
        tree=parse_root_entries(span(struct.unpack_from('<I',pvd,158)[0],struct.unpack_from('<I',pvd,166)[0]))
        cnf=span(*tree['SYSTEM.CNF'])
        if cnf!=b'BOOT = cdrom:SLPS_009.98;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 0X801FFF00\r\n':
            raise ValueError('wrong boot configuration')
        data=span(*tree[BOOT])
    if hashlib.sha256(data).hexdigest()!=EXE_SHA:raise ValueError('wrong Bio Hazard executable')
    return data

# native_span lives in replay_prefix and is re-exported here for existing importers.

def compare_terminal_observations(run,reference,endpoint):
    """Keep independently observed RAM and persisted-card failures distinct."""
    errors=[]
    try:
        actual=terminal_consistency(run/'ram-pages.tsv',run/f'ram-frame-{endpoint:06d}.bin',endpoint)
        ram_match=actual==Path(reference['terminal_ram']).read_bytes()
    except (ValueError,OSError) as error:
        ram_match=False;errors.append('RAM: '+str(error))
    try:
        card_match=source.card_bytes(run/'cards/card1.mcd')==source.card_bytes(reference['terminal_card1'])
    except (ValueError,OSError) as error:
        card_match=False;errors.append('card1: '+str(error))
    return ram_match,card_match,'; '.join(errors) if errors else None

def setup(args):
    if os.name!='nt':raise ValueError('Windows UCRT build required')
    if subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'],text=True).strip():raise ValueError('commit source before building')
    head=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip()
    reference_path=args.reference.resolve(strict=True);reference=verify_reference(reference_path)
    if not 1<=args.jobs<=64:raise ValueError('jobs outside1..64')
    for tool in ('gcc','g++','cmake','ninja','git'):
        if not shutil.which(tool):raise ValueError('missing tool: '+tool)
    bash=Path(shutil.which('git')).resolve().parents[1]/'bin/bash.exe'
    if not bash.is_file():raise ValueError('Git for Windows bash required')
    macros=subprocess.check_output(['gcc','-dM','-E','-include','_mingw.h','-'],input='',text=True)
    if not re.search(r'^#define _UCRT\b',macros,re.M):raise ValueError('UCRT compiler required')
    disc,bios,movie=[p.resolve(strict=True) for p in (args.disc,args.bios,args.movie)]
    track=media(disc,bios);require_hash(movie,source.MOVIE_SHA)
    rows=dualshock_route.read_movie(movie)
    if len(rows)!=source.FRAMES:raise ValueError('incomplete original movie')
    random_receipt_path=args.random_receipt.resolve(strict=True);random_receipt=source.read(random_receipt_path)
    if random_receipt.get('status')!='pass' or random_receipt.get('source_commit')!='ddf225cf63b7b355cb2ac7772450cf473f4b53ac' or random_receipt.get('tape_sha256')!=TAPE_SHA:
        raise ValueError('Nymashock raw random sequence qualification required')
    tape_source=Path(random_receipt['tape']).resolve(strict=True);require_hash(tape_source,TAPE_SHA)
    project=args.project.resolve()
    if project==ROOT or project.is_relative_to(ROOT):raise ValueError('generated candidate must be outside source')
    project.mkdir(parents=True,exist_ok=False)
    data=boot_program(track);exe=project/BOOT;exe.write_bytes(data)
    staged_bios=project/'SCPH5500.BIN';shutil.copyfile(bios,staged_bios);require_hash(staged_bios,source.FIXED['PSX - SCPH5500.BIN'])
    card=project/'initial-card1.mcd';shutil.copyfile(reference['initial_card1'],card);require_hash(card,source.CARD_SHA)
    tape=project/'nymashock-cold-random.psxrng';shutil.copyfile(tape_source,tape);require_hash(tape,TAPE_SHA)
    route=project/'input.psxrti2';receipt=dualshock_route.write_route(rows,route)
    if receipt['canonical_controller_sha256']!=CONTROLLER_SHA:raise ValueError('original controller bytes differ')
    write_json(project/'input.json',receipt)
    # Build cache (docs/tasreplays/build-cache.md): each stage is reused only on an exact
    # content-key match and recorded in setup.json; every check below runs on every path.
    cache_root=build_cache.resolve_root(args)
    common=['-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=gcc','-DCMAKE_CXX_COMPILER=g++']
    tools_argv=lambda tools:['cmake','-S',ROOT/'recompiler','-B',tools,*common,'-DBUILD_TESTING=ON','-DPSXRECOMP_ENABLE_CHD=ON','-DPython3_EXECUTABLE='+sys.executable]
    def build_tools(tools):
        command(tools_argv(tools),project/'configure-tools.log',env=build_cache.build_env())
        command(['cmake','--build',tools,'--parallel',str(args.jobs)],project/'build-tools.log',env=build_cache.build_env())
        command(['ctest','--test-dir',tools,'--output-on-failure','-j',str(args.jobs)],project/'test-tools.log',env=build_cache.build_env())
    tools,tools_record=build_cache.stage_tools(cache_root,ROOT,project,args.tools_dir.resolve() if args.tools_dir else None,
                                              lambda:build_cache.tools_inputs(ROOT,tools_argv(project/'tools')),build_tools,head)
    q=lambda p:json.dumps(p.as_posix())
    bios_profile=project/'bios.toml';profile=(ROOT/'bios/SCPH5500.toml').read_text()
    for key,value in [('rom',staged_bios),('seeds',ROOT/'recompiler/seeds/phase2_ghidra_seeds_SCPH5500.json'),('out_dir',ROOT/'generated')]:
        profile=re.sub(r'^'+key+r'\s*=.*$',lambda _,v=value:key+' = '+q(v),profile,flags=re.M)
    bios_profile.write_text(profile,encoding='utf8')
    game=project/'game.toml'
    game.write_text(f'''[game]
name = "Bio Hazard Director's Cut TAS"
id = "SLPS-00998"
exe = {q(exe)}
load_address = "0x80010000"
entry_pc = "0x800603F8"
text_size = "0x9E800"
stack_base = "0x801FFFF0"
[recompiler]
seeds = {q(project/'seeds.txt')}
bios_config = {q(bios_profile)}
strict = true
out_dir = {q(project/'generated')}
[runtime]
window_title = "Bio Hazard - original TAS"
bios_hle = false
[video]
renderer = "software"
''',encoding='utf8')
    def stamp():
        fingerprint=subprocess.check_output([str(bash),(ROOT/'tools/bios_emitter_fingerprint.sh').as_posix(),bios_profile.as_posix()],cwd=ROOT,text=True).strip()
        if not re.fullmatch('[0-9a-f]{64}',fingerprint):raise ValueError('invalid BIOS fingerprint')
        (ROOT/'generated/SCPH5500.emitter.sha').write_text(fingerprint+'\n')
        return fingerprint
    stamped=[]
    def generate():
        command([tools/'psxrecomp-toml.exe',exe,'--output',project/'census.toml','--seeds',project/'seeds.txt'],project/'census.log')
        command([tools/'psxrecomp-bios.exe','--config',bios_profile,'--rom',staged_bios,'--out-dir',ROOT/'generated'],project/'generate-bios.log')
        stamped.append(stamp())
        command([tools/'psxrecomp-game.exe','--config',game],project/'generate-game.log')
    generated_record=build_cache.stage_generated(cache_root,ROOT,project,'SCPH5500',
        lambda:build_cache.generated_inputs(tools,'SCPH5500',staged_bios,ROOT/'recompiler/seeds/phase2_ghidra_seeds_SCPH5500.json',
                                            build_cache.blob_id(ROOT,'bios/SCPH5500.toml'),build_cache.portable_emitter_fingerprint(bash,ROOT,bios_profile),exe,game),
        True,generate,head)
    # On a cache hit the emitter fingerprint is recomputed and written exactly as on a miss.
    fingerprint=stamped[0] if stamped else stamp()
    native=project/'native'
    native_argv=['cmake','-S',HERE,'-B',native,*common,'-DTAS_PROJECT_DIR='+str(project),
             '-DTAS_GAME_STEM='+BOOT,'-DTAS_EXE_NAME=BioHazard-TAS','-DTAS_WINDOW_TITLE=Bio Hazard TAS',
             '-DPSXRECOMP_BIOS_STEMS=SCPH5500','-DPSX_SHELLWIN_INTERP=ON','-DPSXRECOMP_BIOS_PROFILE='+str(bios_profile),
             '-D_psxrt_bash='+str(bash),'-DPSX_RECOMP_UI=OFF','-DPSX_NETPLAY=OFF','-DPSX_REWIND=OFF','-DPSX_SETUP_WIZARD=OFF',
             '-DPSX_DEBUG_TOOLS=ON','-DPSX_ENABLE_VULKAN=OFF','-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE','-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE']
    def build_native():
        command(native_argv,project/'configure-native.log',env=build_cache.build_env())
        command(['cmake','--build',native,'--parallel',str(args.jobs)],project/'build-native.log',env=build_cache.build_env())
    native_record=build_cache.stage_native(cache_root,ROOT,project,native,'BioHazard-TAS',
        lambda:build_cache.native_inputs(ROOT,build_cache.generated_set(ROOT,'SCPH5500',project),native_argv,bios_profile),build_native,head)
    if subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'],text=True).strip() or subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip()!=head:raise ValueError('source changed during build')
    build=native/'BioHazard-TAS.exe'
    files=[disc,track,bios,staged_bios,bios_profile,movie,exe,game,tape,card,route,project/'seeds.txt',reference_path,random_receipt_path]
    generated={str(p):source.digest(p) for p in (ROOT/'generated').glob('SCPH5500*') if p.is_file()}
    generated.update({str(p):source.digest(p) for p in (project/'generated').glob('*') if p.is_file()})
    info={'schema':'biohazard-tas-candidate-v1','source_head':head,'source_tree':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD^{tree}'],text=True).strip(),
          'reference':str(reference_path),'bindings':[source.bind(p) for p in files],'generated':generated,
          'executable':str(build),'executable_sha256':source.digest(build),'disc':str(disc),'bios':str(staged_bios),'game':str(game),
          'route':str(route),'card1':str(card),'tape':str(tape),'profile':PROFILE,
          'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
          'build_cache':build_cache.receipt_block(cache_root,tools_record,generated_record,native_record),
          'qualification':'candidate only; source/native timing and gameplay unqualified'}
    write_json(project/'setup.json',info);print(json.dumps({'candidate':str(build),'sha256':source.digest(build)}))

def replay(args,returns=None,output=None):
    """One replay: full when returns is None or the endpoint, otherwise a diagnostic prefix."""
    output=(args.output if output is None else Path(output)).resolve()
    setup_path=args.setup.resolve(strict=True);info=source.read(setup_path)
    if info.get('schema')!='biohazard-tas-candidate-v1':raise ValueError('wrong Bio Hazard candidate')
    for binding in info['bindings']:require_hash(Path(binding['path']),binding['sha256'])
    if args.exe is None and args.diagnostic_binary is None:require_hash(Path(info['executable']),info['executable_sha256'])
    binary=resolve_binary(info['executable'],info['executable_sha256'],args.exe,args.diagnostic_binary)
    reference=verify_reference(Path(info['reference']));endpoint=reference['observed_returns']
    if info['profile']!=PROFILE:raise ValueError('candidate profile differs; build a new candidate')
    if output.exists():raise ValueError('fresh output required')
    output.parent.mkdir(parents=True,exist_ok=True)
    wanted=endpoint if returns is None else returns
    records,tail=native_span(source.FRAMES,endpoint,wanted)
    route=Path(info['route']);identity=route_identity(route)
    if identity.get('format')!='PSXRTI2' or identity['frames']!=source.FRAMES or identity['original_controller_sha256']!=CONTROLLER_SHA:
        raise ValueError('candidate does not contain the full original controller route')
    if wanted<endpoint:
        route=write_prefix_route(route,records,output.with_name(output.name+'-input.psxrti2'))
    argv=[sys.executable,str(HERE/'run_native.py'),str(output),'--exe',binary['path'],'--game',info['game'],
          '--route',str(route),'--disc',info['disc'],'--bios',info['bios'],'--card1',info['card1'],
          '--cd-source-clock-tape',info['tape'],'--neutral-tail',str(tail),'--timeout',str(args.timeout),
          '--checkpoint-every','1200','--renderer','software','--storage-budget-mib','3072',
          '--ram-snapshot-frame',str(wanted),*run_native_arguments(args,binary),*PROFILE]
    # Streaming evidence only: it may stop the process early, never decide a pass.
    watcher=Watcher(output,page_reference(Path(reference['ram_pages']),wanted),wanted,stop_on_divergence=args.stop_on_divergence)
    watcher.start()
    try:result=subprocess.run(argv)
    finally:streaming=watcher.finish()
    if not output.is_dir():raise RuntimeError('native launcher rejected before creating its evidence directory')
    comparison=None
    if (output/'ram-pages.tsv').exists():
        # Prefix comparison reads the same independent source, bounded to the
        # diagnostic endpoint; full playback never drops original inputs.
        from compare_ram_pages import read_pages
        from itertools import islice,zip_longest
        first=None;counts=[0,0]
        try:
            for left,right in zip_longest(islice(read_pages(Path(reference['ram_pages'])),wanted),read_pages(output/'ram-pages.tsv')):
                counts[0]+=left is not None;counts[1]+=right is not None
                if first is None and left!=right:
                    first={'frame':(left or right)[0],'source_cycle':left[1] if left else None,'native_cycle':right[1] if right else None,
                           'changed_pages':[f'{i*4096:06X}' for i in range(512) if left and right and left[2][i]!=right[2][i]]}
            comparison={'match':first is None and counts==[wanted,wanted],'returns':counts,'first_divergence':first}
        except (ValueError,OSError) as error:
            comparison={'match':False,'returns':counts,'first_divergence':first,'observation_error':str(error)}
    terminal_match=None;terminal_card_match=None;terminal_error=None
    if wanted==endpoint and result.returncode==0:
        terminal_match,terminal_card_match,terminal_error=compare_terminal_observations(output,reference,wanted)
    mechanical=bool(result.returncode==0 and comparison and comparison['match'] and (wanted<endpoint or (terminal_match and terminal_card_match)))
    status=('fail' if not mechanical else 'diagnostic' if not binary['binary_matches_setup'] else 'prefix_pass' if wanted<endpoint else 'pass')
    receipt={'candidate_sha256':binary['binary_sha256'],**receipt_fields(binary),'source_reference':source.bind(Path(info['reference'])),
             'diagnostic_prefix':wanted<endpoint,'original_input_prefix_unchanged':True,'full_original_input_and_tail':wanted==endpoint,
             'observed_returns':wanted,'native_input_exit':result.returncode,'comparison':comparison,'terminal_ram_match':terminal_match,
             'terminal_card1_match':terminal_card_match,'terminal_observation_error':terminal_error,
             'mechanical_match':mechanical,'status':status,'first_divergence':comparison['first_divergence'] if comparison else None,
             'streaming':streaming,
             'qualification':'mechanical comparison only; ending/semantic review and repeated gameplay remain required'}
    write_json(output/'source-comparison.json',receipt)
    print(json.dumps(receipt))
    return receipt

def run(args):
    check_launch_arguments(args)
    if args.ladder is not None:
        info=source.read(args.setup.resolve(strict=True));endpoint=source.read(Path(info['reference'])).get('observed_returns')
        if type(endpoint) is not int:raise ValueError('invalid source reference endpoint')
        report=run_ladder(args.output.resolve(),parse_ladder(args.ladder,endpoint),endpoint,lambda returns,target:replay(args,returns,target),write_json)
        return 0 if report['status'] in ('pass','prefix_pass') else 1
    receipt=replay(args,args.returns)
    return 0 if receipt['status'] in ('pass','prefix_pass') else 1

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='action',required=True)
    build=sub.add_parser('setup')
    for name in ('project','reference','disc','bios','movie','random-receipt'):build.add_argument('--'+name,type=Path,required=True)
    build.add_argument('--tools-dir',type=Path);build.add_argument('--jobs',type=int,default=4)
    build_cache.add_arguments(build)
    play=sub.add_parser('run');play.add_argument('setup',type=Path);play.add_argument('output',type=Path)
    play.add_argument('--timeout',type=int,default=129600);add_launch_arguments(play)
    args=parser.parse_args();return {'setup':setup,'run':run}[args.action](args)

if __name__=='__main__':raise SystemExit(main())
