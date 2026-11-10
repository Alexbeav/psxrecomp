"""Exercise the existing Studio source route in an isolated private workspace."""
import argparse
import json
import os
from pathlib import Path
import sys
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempt', type=int, default=1)
    parser.add_argument('--no-op', action='store_true')
    args = parser.parse_args()
    if args.no_op:
        print('noop: existing Studio source backend, isolated settings/cache/output, no UI')
        return 0
    core = Path(__file__).resolve().parents[1]
    lane = core.parent
    pair = lane.parent / 'PS1B-165-pair'
    data = pair / f'sd{args.attempt}'
    output = pair / f'so{args.attempt}'
    output.mkdir()
    os.environ['WORKBENCH_SHARED_COMPILER_ROOT'] = str(data / 'shared-compilers')
    os.environ['SDL_AUDIODRIVER'] = 'dummy'
    sys.path.insert(0, str(core / 'tools/public_studio'))
    import backend
    from settings import Settings
    from selection import GameJob
    bios = str(core / 'bios/SCPH1001.BIN')
    preferences = Settings(bios_files=[bios], bios_regions={bios: 'USA'},
                           output_folder=str(output), core_pin='', ui_pin='',
                           custom_core=str(core), custom_ui=str(lane / 'ui'),
                           portable=True, compiler_optimization='fast', lto=False,
                           pgo=False, diagnostics=False, startup_checks=False)
    game = GameJob('Ape Escape Source Pair', [pair / 'p/disc/Ape Escape (USA).cue'])
    runner = backend.Runner(log=lambda line: print(line, flush=True),
                            event=lambda event: print('STUDIO_EVENT ' + json.dumps(event), flush=True))
    timer = threading.Timer(1800, runner.cancel)
    started = time.monotonic()
    record = {'scope': 'real source backend; not GUI or whole-game acceptance',
              'data': str(data), 'output': str(output), 'exit_code': 1}
    timer.start()
    try:
        result = backend.build_batch([game], preferences, data, runner)
        record.update(state=result['state'], sources=[{key: row.get(key) for key in
                      ('kind', 'origin', 'base_commit', 'content_sha256', 'adaptation')}
                     for row in result['sources']], module=result.get('module'), games=result['games'])
        record['exit_code'] = 0 if result['state'] == 'completed' else 1
    except Exception as error:
        record.update(state='failed', error=str(error))
        raise
    finally:
        timer.cancel()
        timer.join()
        record['seconds'] = time.monotonic() - started
        (lane / f'reports/studio-source-pair-{args.attempt}.json').write_text(
            json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'state': record['state'], 'exit_code': record['exit_code'],
                      'seconds': record['seconds'], 'output': str(output)}), flush=True)
    return record['exit_code']


if __name__ == '__main__':
    raise SystemExit(main())
