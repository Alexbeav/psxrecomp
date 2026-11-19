"""The game's player flags (PS1B-687) on the real runtime executable.

  --help / -h        prints every flag and exits 0 before any window, SDL or
                     file write;
  --data-dir <dir>   creates <dir> and puts the files the run writes (memory
                     cards, input.ini, keybinds.ini) and the settings.toml it
                     reads there; nothing new lands beside the exe;
  --set S.K=V        changes the value for the run (read back over the debug
                     port) and is never written to settings.toml; an unknown
                     key is a warning on stderr and the run goes on;
  --fullscreen / --windowed  set the run's window mode and are never written.

Every run is headless (no window, no audio device, dummy SDL audio driver) and
boots the bundled OpenBIOS with no disc. Each run ends with the debug server's
quit command, so exit-time saves happen as in a real run.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--bios', type=Path, required=True)
parser.add_argument('--manifest', type=Path, required=True)
parser.add_argument('--scratch', type=Path, required=True)
args = parser.parse_args()
EXE, BIOS, MANIFEST = args.runtime.resolve(), args.bios.resolve(), args.manifest.resolve()
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
HELP_FLAGS = ['--disc', '--bios', '--game', '--launcher', '--no-launcher', '--game-mode',
              '--no-game-mode', '--fullscreen', '--windowed', '--renderer', '--window-title',
              '--data-dir', '--memcard-dir', '--set', '--netplay', '--net-slot',
              '--net-input-player', '--net-bind', '--net-peer', '--net-delay', '--net-session-id',
              '--netplay-query', '--headless', '--headless-opengl', '--hidden-window',
              '--debug-port', '--replay', '--replay-verdict', '--replay-fast',
              '--launcher-disc-check', '--audit-mod-plugins', '--help', '-h']
failures = []


def check(ok, what):
    print(('PASS ' if ok else 'FAIL ') + what, flush=True)
    if not ok:
        failures.append(what)


def game_folder(root, name):
    folder = root / name
    folder.mkdir(parents=True)
    shutil.copy2(EXE, folder / EXE.name)
    (folder / 'bios').mkdir()
    shutil.copy2(BIOS, folder / 'bios/openbios.bin')
    package = folder / 'mods/bundled/psx.integration.debug-server/1.0.0'
    package.mkdir(parents=True)
    shutil.copy2(MANIFEST, package / 'manifest.toml')
    return folder


def listing(folder):
    return sorted(str(p.relative_to(folder)) for p in folder.rglob('*'))


def request(port, command):
    with socket.create_connection(('127.0.0.1', port), timeout=3) as sock:
        sock.sendall(json.dumps({'id': 7, **command}).encode() + b'\n')
        return json.loads(sock.makefile('rb').readline())


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def headless_run(folder, extra, logs, until=None):
    """Start headless, wait for the debug port (and `until`), query menu_host,
    quit. Returns (menu_host reply or None, stdout, stderr)."""
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', PSX_DEBUG_SERVER='1',
               TEMP=str(logs), TMP=str(logs))
    for name in ('PSX_VOLUME', 'PSX_HEADLESS', 'PSX_NO_LAUNCHER'):
        env.pop(name, None)
    port = free_port()
    out_path, err_path = logs / (folder.name + '.out'), logs / (folder.name + '.err')
    reply = None
    with out_path.open('wb') as out, err_path.open('wb') as err:
        proc = subprocess.Popen([str(folder / EXE.name), '--headless', '--bios', str(BIOS),
                                 '--debug-port', str(port)] + extra,
                                cwd=logs, env=env, stdout=out, stderr=err, creationflags=NO_WINDOW)
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline and proc.poll() is None:
                try:
                    reply = request(port, {'cmd': 'menu_host'})
                    if until is None or until():
                        break
                except OSError:
                    pass
                time.sleep(0.2)
            if proc.poll() is None:
                try:
                    request(port, {'cmd': 'quit'})
                except OSError:
                    pass
                proc.wait(timeout=15)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
    return (reply, out_path.read_text(encoding='utf-8', errors='replace'),
            err_path.read_text(encoding='utf-8', errors='replace'))


with tempfile.TemporaryDirectory(prefix='cli-player-flags-', dir=args.scratch) as scratch:
    root = Path(scratch)
    logs = root / 'logs'
    logs.mkdir()

    # --help and -h: usage on stdout, exit 0, no file written, quick.
    for flag in ('--help', '-h'):
        folder = game_folder(root, 'help' + flag.replace('-', '_'))
        before = listing(folder)
        start = time.monotonic()
        try:
            # --headless too: a build that does not know the flag must still
            # open no window or audio while it is being judged.
            run = subprocess.run([str(folder / EXE.name), flag, '--headless'], cwd=logs, capture_output=True,
                                 timeout=20, text=True, encoding='utf-8', errors='replace',
                                 creationflags=NO_WINDOW,
                                 env=dict(os.environ, SDL_AUDIODRIVER='dummy', TEMP=str(logs), TMP=str(logs)))
            code, text = run.returncode, run.stdout
        except subprocess.TimeoutExpired:
            code, text = 'timeout', ''
        elapsed = time.monotonic() - start
        missing = [f for f in HELP_FLAGS if f not in text]
        check(code == 0, f'{flag} exits 0 (got {code}, {elapsed:.1f} s)')
        check(not missing, f'{flag} lists every flag (missing: {missing})')
        check(listing(folder) == before, f'{flag} writes nothing beside the exe')

    # --data-dir with a folder that does not exist yet.
    folder = game_folder(root, 'data_new')
    before = listing(folder)
    data = folder / 'player data'
    _, out, err = headless_run(folder, ['--data-dir', 'player data'], logs,
                               until=lambda: (data / 'card1.mcd').is_file())
    check(data.is_dir(), '--data-dir creates the missing folder (relative to the exe)')
    check((data / 'card1.mcd').is_file() and (data / 'card2.mcd').is_file(),
          '--data-dir puts the memory cards in the data folder')
    new_beside_exe = [p for p in listing(folder) if p not in before and not p.startswith('player data')]
    check(not new_beside_exe, f'--data-dir writes nothing new beside the exe (new: {new_beside_exe})')

    # --data-dir reads settings.toml from the data folder, not the exe folder.
    folder = game_folder(root, 'data_settings')
    (folder / 'settings.toml').write_text('[audio]\nvolume = 11\n', encoding='utf-8')
    data = root / 'abs data'
    data.mkdir()
    (data / 'settings.toml').write_text('[audio]\nvolume = 37\n', encoding='utf-8')
    reply, out, err = headless_run(folder, ['--data-dir', str(data)], logs)
    check(reply is not None and reply.get('volume') == 37,
          f'--data-dir reads the data folder settings.toml (volume {reply and reply.get("volume")}, want 37)')

    # --set: changes the run, never the file; unknown key warns and the run goes on.
    folder = game_folder(root, 'set')
    settings = folder / 'settings.toml'
    settings.write_bytes(b'[audio]\nvolume = 20\n\n[video]\nfullscreen = 0\n')
    original = settings.read_bytes()
    reply, out, err = headless_run(folder, ['--set', 'audio.volume=64', '--set', 'bogus.key=1'], logs)
    check(reply is not None and reply.get('volume') == 64,
          f'--set audio.volume=64 applies to the run (volume {reply and reply.get("volume")})')
    check('--set bogus.key=1 ignored' in err, '--set with an unknown key warns on stderr')
    check(settings.read_bytes() == original, '--set leaves settings.toml as it was')

    # --fullscreen / --windowed: the run's window mode, never written.
    for flag, stored, want in (('--fullscreen', 0, 'window mode = fullscreen (command line'),
                               ('--windowed', 1, 'window mode = windowed (command line')):
        folder = game_folder(root, 'mode' + flag.replace('-', '_'))
        settings = folder / 'settings.toml'
        settings.write_bytes(b'[video]\nfullscreen = %d\n' % stored)
        original = settings.read_bytes()
        reply, out, err = headless_run(folder, [flag], logs)
        check(want in out, f'{flag} sets the window mode for the run')
        check(settings.read_bytes() == original, f'{flag} leaves settings.toml as it was')

print('FAIL: %d check(s)' % len(failures) if failures else 'PASS: all player flag checks')
sys.exit(1 if failures else 0)
