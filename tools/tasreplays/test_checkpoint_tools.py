"""Synthetic controls for the checkpoint tools: no game, disc or BIOS.

A stand-in game (this file run with --fake-game) follows the runtime's
checkpoint contract: it captures at the requested frames, defers the capture
requested at 500 by two frames as a boundary would, resumes from a state,
applies the timer2 perturbation, and quits on the debug server's quit command.

Pin J ships PR #923's run_checkpoint.py and compare_boot_states.py but keeps
pin I's compare_checkpoint_runs.py and verify_checkpoint_cohort.py (PS1B-685),
so this test covers the two PR #923 tools only (PS1B-702). It also checks
which load path run_native.py gives its checkpoint options.
"""
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import time
import zlib

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from compare_boot_states import compare, sections  # noqa: E402
from run_checkpoint import main as run_main, run  # noqa: E402
from run_native import checkpoint_load_path  # noqa: E402

DEFERRED = {500: 2}


def image(frame, timer_offset, compressed=True, extra=b''):
    ram = bytes((frame * 7 + i) & 0xFF for i in range(4096))
    body = [(0x01, 0, struct.pack('<I', frame)),
            (0x05, 0, struct.pack('<II', (frame * 3 + timer_offset) & 0xFFFF, timer_offset))]
    if compressed:
        body.append((0x02, 1, struct.pack('<I', len(ram)) + zlib.compress(ram)))
    else:
        body.append((0x02, 0, ram))
    out = struct.pack('<9I', 0x50535842, 12, 1, 2, 3, 4, 5, len(body), 0)
    for tag, flags, payload in body:
        out += struct.pack('<IIQ', tag, flags, len(payload)) + payload
    return out + extra


def fake_game(port, mode):
    """mode: honest, lossy (a resume drops the timer offset) or blind (ignores
    the perturbation)."""
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', port))
    server.listen(1)
    quit_event = threading.Event()

    def serve():
        conn, _ = server.accept()
        with conn:
            if b'"quit"' in conn.recv(256):
                conn.sendall(b'{"ok":true}\n')
                quit_event.set()
    threading.Thread(target=serve, daemon=True).start()

    save_at = sorted(int(x) for x in os.environ['PSX_TAS_SAVE_STATE_AT'].split(','))
    path_tpl = os.environ['PSX_TAS_SAVE_STATE_PATH']
    frame, timer_offset = 0, 5  # state a cold boot sets and a resume must restore
    resume = os.environ.get('PSX_TAS_RESUME_STATE')
    if resume:
        manifest = Path(resume + '.json')
        if not manifest.exists():
            print('resume rejected: no manifest', file=sys.stderr)
            return 2
        frame = json.loads(manifest.read_text())['frame']
        _, secs = sections(resume)
        timer_offset = 0 if mode == 'lossy' else struct.unpack_from('<II', secs[0x05])[1]
        if os.environ.get('PSX_TAS_PERTURB_RESTORE') == 'timer2' and mode != 'blind':
            timer_offset += 1
    pending = None  # (requested, take_at)
    deadline = time.monotonic() + 30
    while not quit_event.is_set() and time.monotonic() < deadline:
        if frame < save_at[-1] + 10:
            frame += 1
            if pending is None and frame in save_at:
                pending = (frame, frame + DEFERRED.get(frame, 0))
            if pending and frame >= pending[1]:
                requested, pending = pending[0], None
                state = Path(path_tpl.replace('{frame}', f'{frame:06d}'))
                data = image(frame, timer_offset)
                state.write_bytes(data)
                state.with_name(state.name + '.json').write_text(json.dumps({
                    'schema': 'psx-tas-stateio-v3', 'frame': frame, 'requested_frame': requested,
                    'cycle': frame * 1000 + timer_offset, 'ram_digest': f'{frame:016X}'}))
        else:
            time.sleep(0.01)
    return 0


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def game_command(port, mode='honest'):
    return [sys.executable, '-B', str(Path(__file__).resolve()), '--fake-game', str(port), mode]


def check(ok, what):
    if not ok:
        raise AssertionError(what)


def test_compare_boot_states(tmp):
    a, b, c = tmp / 'a.pst', tmp / 'b.pst', tmp / 'c.pst'
    a.write_bytes(image(10, 0))
    b.write_bytes(image(10, 0, compressed=False))
    c.write_bytes(image(10, 1))
    check(compare(a, b)['identical'], 'compression does not change the comparison')
    diff = compare(a, c)['differences']
    check([d['section'] for d in diff] == ['TIMER'] and diff[0]['offset'] == 0,
          'a changed section is named with its first differing offset')
    bad = tmp / 'bad.pst'
    for name, data in (('trailing bytes', image(10, 0, extra=b'x')),
                       ('repeated section', struct.pack('<9I', 0x50535842, 12, 1, 2, 3, 4, 5, 2, 0)
                        + 2 * (struct.pack('<IIQ', 0x01, 0, 4) + b'\0' * 4))):
        bad.write_bytes(data)
        try:
            sections(bad)
        except ValueError:
            continue
        raise AssertionError(f'{name} is refused')


def main(argv):
    if argv[1:2] == ['--fake-game']:
        return fake_game(int(argv[2]), argv[3])
    # run_native.py: only the source GPU model takes stateio; every other model
    # would take PR #923's frame path, which cannot seek a route, and is refused.
    quad = 'octoshock-2.2.2-bounded-quad'
    check(checkpoint_load_path('default', None, None) == 'none', 'no checkpoint option, no load path')
    check(checkpoint_load_path(quad, [300], None) == 'stateio', 'bounded-quad captures through stateio')
    check(checkpoint_load_path(quad, None, Path('s.pst')) == 'stateio', 'bounded-quad resumes through stateio')
    for model in ('default', 'octoshock-2.2.2-vram-upload', 'octoshock-2.2.2-bounded-linked-list'):
        for save_at, resume in (([300], None), (None, Path('s.pst'))):
            try:
                checkpoint_load_path(model, save_at, resume)
            except ValueError as error:
                check(quad in str(error), f'{model}: the refusal names the model it needs')
                continue
            raise AssertionError(f'{model}: a checkpoint option is refused')
    with tempfile.TemporaryDirectory(prefix='checkpoint-tools-') as name:
        tmp = Path(name)
        test_compare_boot_states(tmp)

        # A capture requested at 500 is deferred to 502 and names both frames.
        port = free_port()
        code, rows = run(tmp / 'cold', game_command(port), [500, 600], None, None, port, 30)
        check(code == 0, f'the cold run reaches its last capture, got {code}')
        check([(r['requested_frame'], r['frame']) for r in rows] == [(500, 502), (600, 600)],
              f'captures are taken where the game took them: {rows}')
        cold = tmp / 'cold'

        # A resume from the deferred capture reproduces the cold run at 600;
        # the timer2 perturbation is caught there, and a lossy resume too.
        for name, mode, perturb, same in (('resume', 'honest', None, True),
                                          ('negative', 'honest', 'timer2', False),
                                          ('lossy', 'lossy', None, False)):
            port = free_port()
            code, rows = run(tmp / name, game_command(port, mode), [600],
                             cold / 'tas-state-000502.pst', perturb, port, 30)
            check(code == 0 and [r['frame'] for r in rows] == [600], f'{name}: exit {code}, {rows}')
            result = compare(cold / 'tas-state-000600.pst', tmp / name / 'tas-state-000600.pst')
            check(result['identical'] == same, f'{name}: identical={result["identical"]}')
            if not same:
                check([d['section'] for d in result['differences']] == ['TIMER'],
                      f'{name}: the timer section is named: {result["differences"]}')

        # A refused resume ends the run with the game's status.
        port = free_port()
        code, _ = run(tmp / 'refused', game_command(port), [600], tmp / 'nothing.pst', None, port, 30)
        check(code == 2, f'a refused resume returns the game status, got {code}')

        # The documented run command line, options after the output folder.
        port = free_port()
        code = run_main([str(tmp / 'cli'), '--save-at', '600', '--port', str(port), '--timeout', '30',
                         '--', *game_command(port)])
        check(code == 0 and json.loads((tmp / 'cli' / 'captures.json').read_text())[0]['requested_frame'] == 600,
              f'the run command line captures at the requested frame, got exit {code}')
    print('checkpoint tools: state comparison, run driver, resume controls and run_native load path pass')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
