"""Bounded player-recorder check on the actual private modern consumer.
This tests its own recording/playback; it is not a programme TAS/oracle gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import threading
import time

sys.stdout.reconfigure(encoding='utf-8',errors='replace')
sys.stderr.reconfigure(encoding='utf-8',errors='replace')
sys.path.insert(0,str(Path(__file__).resolve().parent/'public_studio'))
from backend import Runner, Cancelled


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=('record','play'))
    parser.add_argument('--attempt',type=int,default=1)
    parser.add_argument('--build-name',choices=('b','n'),default='b')
    parser.add_argument('--product-dir',type=Path)
    parser.add_argument('--anchor-vblank',type=int,default=0)
    parser.add_argument('--no-op',action='store_true')
    args=parser.parse_args()
    if args.no_op:
        print('noop: actual headless recorder with a bounded owned child tree');return 0
    core=Path(__file__).resolve().parents[1]
    lane=core.parent
    pair=lane.parent/'PS1B-165-pair'
    qa=pair/'qa'/f'{args.mode}-{args.attempt}'
    if qa.exists():raise RuntimeError('Evidence exists; use a new attempt')
    qa.mkdir(parents=True)
    # Headless still exposes the normal debug socket. Do not collide with a user process.
    with socket.socket() as check:
        check.bind(('127.0.0.1',57791))
    exe=pair/args.build_name/'Ape_Modern_Pair.exe'
    if args.product_dir:
        product=args.product_dir.resolve()
        executables=list(product.glob('*.exe'))
        if len(executables)!=1 or not (product/'game.toml').is_file():
            raise RuntimeError('Expected one delivered game executable and game.toml')
        exe=executables[0]
    command=[exe,'--headless','--no-launcher','--renderer','software','--debug-port','57791',
             '--game',pair/'p/game.toml','--disc',pair/'p/disc/Ape Escape (USA).cue',
             '--bios',core/'bios/SCPH1001.BIN','--memcard-dir',qa/'cards']
    if args.product_dir:
        # Exercise delivered config/media resolution, without source-path overrides.
        command=[exe,'--headless','--no-launcher','--renderer','software',
                 '--debug-port','57791','--game',product/'game.toml','--memcard-dir',qa/'cards']
    env=os.environ.copy()
    for key in list(env):
        if key.startswith('PSX_REPLAY_'):env.pop(key)
    env.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy',PSX_REPLAY_EXIT_AT_END='1',
               PYTHONDONTWRITEBYTECODE='1',TEMP=str(pair/'tmp'),TMP=str(pair/'tmp'))
    anchor_vblank=args.anchor_vblank
    if args.mode=='record':
        env.update(PSX_REPLAY_RECORD_FRAMES='600',PSX_REPLAY_TEST_INPUT_SEED='165')
        if args.anchor_vblank:
            if args.anchor_vblank < 0:raise ValueError('Anchor vblank must be nonnegative')
            env.update(PSX_REPLAY_RECORD_FILE=str(qa/'anchor.psxrpl'),
                       PSX_REPLAY_RECORD_AT=str(args.anchor_vblank))
        else:
            env['PSX_REPLAY_RECORD_BOOT']='1'
    else:
        record=json.loads((lane/f'reports/modern-player-record-{args.build_name}-{args.attempt}.json').read_text(encoding='utf-8'))
        anchor_vblank=record.get('anchor_vblank',0)
        command+=['--replay',record['replay'],'--replay-verdict',qa/'verdict.json','--replay-fast']
    runner=Runner(log=lambda line: print(line,flush=True))
    timer=threading.Timer(180,runner.cancel)
    started=time.monotonic();code=0;error=''
    timer.start()
    try:runner.run(command,exe.parent,env=env)
    except Cancelled as exc:code=124;error=str(exc)
    except RuntimeError as exc:code=1;error=str(exc)
    finally:timer.cancel();timer.join()
    record={'mode':args.mode,'attempt':args.attempt,'exit_code':code,'seconds':time.monotonic()-started,
            'command':[str(value) for value in command],'qa':str(qa),'error':error,
            'delivered_product':str(args.product_dir.resolve()) if args.product_dir else None}
    record['anchor_vblank']=anchor_vblank
    record['source_profile']={key:env[key] for key in (
        'PSX_GPU_DMA_MODEL','PSX_INPUT_ROUTE_FILE','PSX_INPUT_ROUTE_FIELD_MODEL',
        'PSX_GPU_STATUS_MODEL','PSX_CD_DMA_MODEL','PSX_INPUT_ROUTE_DMA_MODEL',
        'PSX_MDEC_SOURCE_MODEL','PSX_CD_CDDA_MODEL','PSX_CD_COLD_STATUS_MODEL',
        'PSX_CD_DRIVE_MODEL','PSX_CD_EXPLICIT_SEEK_MODEL','PSX_CD_FIRMWARE_MODEL',
        'PSX_CD_INIT_DRAW_ORDER','PSX_CD_READ_START_MODEL','PSX_CD_SOURCE_CLOCK_TAPE',
        'PSX_CD_TOC_SEEK_MODEL') if key in env}
    with open(exe,'rb') as stream:
        record['exe_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
    if not code and args.mode=='record':
        files=list(qa.rglob('*.psxrpl'))
        if len(files)!=1:
            code=record['exit_code']=1
            record['error']=f'Expected one complete replay; found {len(files)} under {qa}'
        else:
            record['replay']=str(files[0]);record['bytes']=files[0].stat().st_size
    if not code and args.mode=='play':
        verdict=json.loads((qa/'verdict.json').read_text(encoding='utf-8'))
        record['verdict']=verdict
        if verdict.get('result')!='in_sync' or not verdict.get('digests_checked'):
            code=record['exit_code']=1
    (lane/f'reports/modern-player-{args.mode}-{args.build_name}-{args.attempt}.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(record,indent=2),flush=True)
    return code


if __name__=='__main__':raise SystemExit(main())
