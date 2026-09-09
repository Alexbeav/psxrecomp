"""Cold Bio Hazard save-route authoring using controller input and read-only observations.

This is separate from the original TAS. A session optionally replays an unchanged
original prefix before authoring, or cold-boots a fresh copy of an existing raw
card without a movie. Only game execution can change card contents.
"""
from pathlib import Path
import argparse,csv,datetime,hashlib,json,os,shutil,stat,subprocess,sys,time
import biohazard
import dualshock_route
import nymashock_admission as admission

HERE=Path(__file__).resolve().parent
BITS={'select':0,'l3':1,'r3':2,'start':3,'up':4,'right':5,'down':6,'left':7,
      'l2':8,'r2':9,'l1':10,'r1':11,'triangle':12,'circle':13,'cross':14,'square':15}

def write(path,data):
    with Path(path).open('x',encoding='utf8') as f:json.dump(data,f,indent=2);f.write('\n')

def card(path):
    data=Path(path).read_bytes()
    if len(data)!=131072 or data[:2]!=b'MC':raise ValueError('raw 128 KiB PSX card required')
    return admission.bind(path)

def controller_command(frames,press,axes):
    if type(frames) is not int or not 1<=frames<=12000:raise ValueError('step must contain1..12000 frames')
    if len(axes)!=4 or any(type(x) is not int or not 0<=x<=255 for x in axes):raise ValueError('four byte axes required in LY,LX,RY,RX order')
    if len(set(press))!=len(press) or any(x not in BITS for x in press):raise ValueError('unknown or repeated controller button')
    word=65535
    for name in press:word&=~(1<<BITS[name])
    return ' '.join(map(str,[frames,word,*axes]))+'\n'

def read_controller(path,endpoint):
    rows=[]
    with Path(path).open() as f:
        if f.readline().strip()!='frame\tbuttons\tly\tlx\try\trx\tanalog':raise ValueError('wrong controller columns')
        for n,line in enumerate(f,1):
            values=[int(x) for x in line.split()]
            if len(values)!=7 or values[0]!=n or values[-1]!=0:raise ValueError('invalid/discontinuous observed controller input')
            rows.append(tuple(values[1:]))
            if n>300000:raise ValueError('session frame limit')
    if len(rows)!=endpoint:raise ValueError('incomplete controller route')
    return rows

def helper_identity(receipt_path):
    receipt=admission.read(receipt_path)
    if receipt.get('status')!='compiled' or receipt.get('exit_code')!=0:raise ValueError('compiled progression helper required')
    for item in receipt['bindings']:
        biohazard.require_hash(Path(item['path']),item['sha256'])
    helper=Path(receipt['dll']).resolve(strict=True)
    required={str(p.resolve()) for p in [HERE/'Progression291.cs',helper]}
    if not required<={str(Path(x['path']).resolve()) for x in receipt['bindings']}:raise ValueError('unbound progression helper')
    return helper

def build_helper(reference,output):
    ref=biohazard.verify_reference(reference)
    previous=admission.read(Path(ref['stock_source'])/'observer-build.json')
    for path,digest in previous['references'].items():biohazard.require_hash(Path(path),digest)
    compiler=Path(previous['argv'][0]);biohazard.require_hash(compiler,previous['compiler_sha256'])
    output=Path(output).resolve();output.mkdir(parents=True,exist_ok=False)
    dll=output/'Progression291.dll'
    argv=[str(compiler),'/nologo','/target:library','/optimize+','/out:'+str(dll),
          *[v for v in previous['argv'] if v.startswith('/r:')],str(HERE/'Progression291.cs')]
    result=subprocess.run(argv,capture_output=True)
    (output/'stdout.log').write_bytes(result.stdout);(output/'stderr.log').write_bytes(result.stderr)
    receipt={'status':'compiled' if result.returncode==0 else 'failed','exit_code':result.returncode,'argv':argv,
             'dll':str(dll),'bindings':[admission.bind(p) for p in [reference,compiler,HERE/'Progression291.cs',
                         *map(Path,previous['references']),*([dll] if result.returncode==0 else [])]],
             'qualification':'compile only; prefix equivalence, loaded-card identity and save/load behavior require execution'}
    write(output/'receipt.json',receipt)
    if result.returncode:raise RuntimeError('progression helper compilation failed')
    return receipt

def queue(run,frames,press,axes,finish=False):
    run=Path(run).resolve(strict=True)
    if (run/'exit.json').exists() or (run/'complete.json').exists():raise ValueError('session has ended')
    ready=admission.read(run/'ready.json');step=ready['completed_step']+1
    if not 1<=step<=4096 or (not finish and ready['frame']+frames>300000):raise ValueError('session bound exceeded')
    payload='finish\n' if finish else controller_command(frames,press,axes)
    target=run/'commands'/f'{step:06d}.txt'
    if target.exists():raise ValueError('wait for the pending step to complete')
    temporary=target.with_suffix('.pending')
    with temporary.open('x',encoding='ascii',newline='\n') as f:f.write(payload)
    write(run/'commands'/f'{step:06d}.json',{'step':step,'frame_before':ready['frame'],'controller_command':payload,
          'utc':datetime.datetime.now(datetime.timezone.utc).isoformat()})
    # Windows rename refuses an existing destination; never replace a command.
    temporary.rename(target)
    return {'queued_step':step,'frame_before':ready['frame'],'payload':payload}

def run(args):
    if os.name!='nt':raise ValueError('Windows stock source host required')
    listing=subprocess.check_output(['tasklist','/FI','IMAGENAME eq EmuHawk.exe','/FO','CSV','/NH'],text=True)
    if any(row and row[0].lower()=='emuhawk.exe' for row in csv.reader(listing.splitlines())):
        raise ValueError('finish the existing source emulator before starting another')
    reference_path=args.reference.resolve(strict=True);ref=biohazard.verify_reference(reference_path)
    helper=helper_identity(args.helper_build)
    if not 0<=args.prefix<=227202 or not 60<=args.timeout<=129600:raise ValueError('invalid prefix or timeout')
    if args.prefix and args.card:raise ValueError('original prefix starts with the original blank card')
    if not args.prefix and not args.card:raise ValueError('cold save load requires an explicit card')
    initial=Path(ref['initial_card1']) if args.prefix else args.card.resolve(strict=True)
    initial_binding=card(initial)
    stock=Path(ref['stock_source']);manifest=admission.read(stock/'manifest.json')
    bound={Path(x['path']).name:Path(x['path']) for x in manifest['bindings']}
    app=bound['EmuHawk.exe'];bios=bound['PSX - SCPH5500.BIN'];cue=bound["Bio Hazard - Director's Cut (Japan).cue"]
    movie=bound['re1-original.bk2']
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    for name in ('commands','SaveRAM','State','Screenshots','MovieBackups'):(output/name).mkdir()
    staged=output/'initial-card1.mcd';shutil.copyfile(initial,staged)
    if not args.prefix:shutil.copyfile(staged,output/'SaveRAM'/(cue.stem+'.SaveRAM'))
    frozen=output/'re1-original.bk2'
    if args.prefix:shutil.copyfile(movie,frozen);frozen.chmod(stat.S_IREAD)
    config=admission.read(stock/'launch-config.json')
    config['PreferredCores']={'PSX':'Nymashock'}
    config['FirmwareUserSpecifications']={'PSX+J':str(bios)}
    config['PathEntries']={'Paths':[{'System':'PSX','Type':t,'Path':str(output/d)}
        for t,d in [('Save RAM','SaveRAM'),('Savestates','State'),('Screenshots','Screenshots')]]}
    write(output/'host-config.json',config);write(output/'launch-config.json',config)
    for name in ('nymashock_progression.py','nymashock_progression.lua','Progression291.cs'):shutil.copyfile(HERE/name,output/name)
    (output/'start.lua').write_text('ROUTE_ROOT=[['+output.as_posix()+']]\nROUTE_PREFIX='+str(args.prefix)+
        '\nROUTE_HELPER=[['+helper.as_posix()+']]\nROUTE_CORE_SHA="'+admission.FIXED['BizHawk.Emulation.Cores.dll']+
        '"\nROUTE_WBX_SHA="'+admission.FIXED['shock.wbx.zst']+'"\nROUTE_CARD_SHA="'+initial_binding['sha256']+'"\n'+
        (output/'nymashock_progression.lua').read_text(),encoding='utf8')
    command=[str(app),str(cue),'--config='+str(output/'host-config.json')]
    if args.prefix:command+=['--movie='+str(frozen)]
    command+=['--lua='+str(output/'start.lua')]
    files=[reference_path,args.helper_build,helper,initial,staged,output/'start.lua',output/'launch-config.json',
           *[output/n for n in ('nymashock_progression.py','nymashock_progression.lua','Progression291.cs')]]
    if args.prefix:files.append(frozen)
    write(output/'manifest.json',{'schema':'biohazard-source-progression-session-v1','command':command,
        'bindings':[admission.bind(p) for p in files],'original_prefix':args.prefix,'initial_card':initial_binding,
        'max_bytes':3*1024**3,'max_files':5000,'timeout_seconds':args.timeout,
        'scope':'Authored controller route; no full original TAS completion claim; semantic save/load admission separate'})
    startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    start=time.monotonic();reason=None
    with (output/'stdout.log').open('xb') as out,(output/'stderr.log').open('xb') as err:
        process=subprocess.Popen(command,cwd=app.parent,stdout=out,stderr=err,startupinfo=startup)
        write(output/'process.json',{'pid':process.pid});print(json.dumps({'pid':process.pid,'session':str(output)}),flush=True)
        while process.poll() is None:
            files=[p for p in output.rglob('*') if p.is_file()]
            if time.monotonic()-start>args.timeout:reason='host_timeout'
            elif len(files)>5000 or sum(p.stat().st_size for p in files)>3*1024**3:reason='storage_budget'
            if reason:process.terminate();process.wait(timeout=20);break
            time.sleep(2)
    result={'exit_code':process.returncode,'stop_reason':reason,'host_seconds':time.monotonic()-start,
            'source_card_unchanged':admission.digest(initial)==initial_binding['sha256'],
            'original_movie_unchanged':admission.digest(movie)==admission.MOVIE_SHA and
                (not args.prefix or admission.digest(frozen)==admission.MOVIE_SHA),'route_complete':False}
    try:
        if process.returncode or reason:raise ValueError('source session did not finish cleanly')
        complete=admission.read(output/'complete.json');rows=read_controller(output/'controller.tsv',complete['frame'])
        if args.prefix and rows[:args.prefix]!=dualshock_route.read_movie(movie)[:args.prefix]:raise ValueError('original source prefix controller bytes differ')
        identity=dualshock_route.write_route(rows,output/'authored-input.psxrti2')
        write(output/'authored-input.json',{**identity,'route':admission.bind(output/'authored-input.psxrti2'),
                'original_prefix':args.prefix,'qualification':'Controller route only; save contents and fresh load need review'})
        result['route_complete']=True
    except (ValueError,OSError) as error:result['route_error']=str(error)
    write(output/'exit.json',result)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='action',required=True)
    build=sub.add_parser('build-helper');build.add_argument('reference',type=Path);build.add_argument('output',type=Path)
    session=sub.add_parser('run')
    for name in ('reference','helper-build','output'):session.add_argument('--'+name,type=Path,required=True)
    session.add_argument('--card',type=Path);session.add_argument('--prefix',type=int,default=0);session.add_argument('--timeout',type=int,default=7200)
    step=sub.add_parser('step');step.add_argument('run',type=Path);step.add_argument('frames',type=int)
    step.add_argument('--press',nargs='*',default=[]);step.add_argument('--axes',nargs=4,type=int,default=[128]*4)
    finish=sub.add_parser('finish');finish.add_argument('run',type=Path)
    args=parser.parse_args()
    if args.action=='build-helper':result=build_helper(args.reference,args.output)
    elif args.action=='run':
        result=run(args);print(json.dumps(result));return 0 if result['route_complete'] and result['source_card_unchanged'] and result['original_movie_unchanged'] else 1
    elif args.action=='step':result=queue(args.run,args.frames,args.press,args.axes)
    else:result=queue(args.run,0,[],[],finish=True)
    print(json.dumps(result));return 0

if __name__=='__main__':raise SystemExit(main())
