#!/usr/bin/env python3
"""Player-replay determinism check (PS1B-191) on a real title.

Builds the Tekken 3 (USA) player from THIS checkout at -O0 and -O2 (release
product, no debug tools), then for each build:
  1. record: boot headless, and from vblank K record N frames of scripted P1
     input (PSX_REPLAY_TEST_INPUT_SEED) into a replay file;
  2. play: a fresh process loads the replay's anchor, feeds the recorded P1 and
     compares cycle count and main-RAM SHA-256 at the END boundary
     (exit 0 = in sync, 3 = out of sync);
  3. negative control: the same replay with one recorded input changed must
     end out of sync, so the comparison is known to see a divergence.
When a playback drifts, the report keeps the replay_end line (cycle delta and
differing RAM pages) instead of passing.

With --power-on the same build checks the launcher's "Record replay" path
(PS1B-316, PSX_REPLAY_RECORD_BOOT=1): a power-on replay from vblank 0 of N
frames, with no anchor, into <saves>/replays/<title>-boot-<UTC>.psxrpl.
  1. record twice with the same inputs: the two files must be identical apart
     from their name entry (the A/B "no behaviour change" comparison);
  2. record once more with a different card in slot 1, to show whether the
     title reads its card within N frames (reported, not judged);
  3. play the first recording twice, with the player's card 1 replaced by a
     different card: both must end in sync, the verdict must name the power-on
     start and this exe's SHA-256, and the player's card file must not change;
  4. negative control: one recorded input changed ends out of sync.

usage: run.py --disc "Tekken 3 (USA).chd" --bios SCPH1001.BIN --output PRIVATE_NEW_DIR
              [--chdman chdman.exe] [--jobs N] [--opt O0 O2] [--record-at K] [--frames N]
              [--power-on]
The output holds generated code from a retail disc: keep it on private storage.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TAS = ROOT / 'tools' / 'tasreplays'
sys.path.insert(0, str(TAS))
import tekken3  # noqa: E402  (disc checks, EXE extraction, pinned hashes)
import media_container  # noqa: E402


def sha(path: Path) -> str:
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def run(argv, log: Path, env=None, cwd=None, timeout=None) -> int:
    with log.open('w', encoding='utf8') as out:
        out.write(json.dumps([str(a) for a in argv]) + '\n')
        out.flush()
        p = subprocess.run([str(a) for a in argv], stdout=out, stderr=subprocess.STDOUT,
                           env=env, cwd=cwd, timeout=timeout)
    return p.returncode


NAME_TAG = 0x80000305   # INPUT_ROUTE_TAG_REPLAY_NAME: carries the wall-clock start


def replay_parts(path: Path) -> tuple[bytes, list[tuple[int, bytes]], bytes]:
    """Header, extension entries (tag, payload) and records of a PSXRTI3 file."""
    data = path.read_bytes()
    assert data[:8] == b'PSXRTI3\0', path
    ext = int.from_bytes(data[24:28], 'little')
    entries, at = [], 28
    while at < 28 + ext:
        tag = int.from_bytes(data[at:at + 4], 'little')
        length = int.from_bytes(data[at + 4:at + 8], 'little')
        entries.append((tag, data[at + 8:at + 8 + length]))
        at += 8 + ((length + 3) & ~3)
    return data[:28], entries, data[28 + ext:]


def same_replay(a: Path, b: Path) -> bool:
    """Byte-identical apart from the NAME entry (and so the header's size)."""
    ha, ea, ra = replay_parts(a)
    hb, eb, rb = replay_parts(b)
    keep = lambda entries: [e for e in entries if e[0] != NAME_TAG]
    return ha[:24] == hb[:24] and keep(ea) == keep(eb) and ra == rb


def end_checkpoint(path: Path) -> bytes:
    """The END checkpoint entry (cycle, RAM SHA-256 and page hashes)."""
    return [p for t, p in replay_parts(path)[1] if t == 0x80000111][-1]


def power_on_runs(exe: Path, built: dict, runs: Path, args) -> tuple[dict, bool]:
    """PS1B-316: record and play power-on replays with one build (see the module doc)."""
    cards = ROOT / 'runtime/tests/test_card_synth.mcd'
    base = lambda saves: [exe, '--headless', '--game', built['game'], '--disc', built['disc'],
                          '--bios', built['bios'], '--memcard-dir', saves]
    record_env = {'PSX_REPLAY_RECORD_BOOT': '1', 'PSX_REPLAY_RECORD_FRAMES': str(args.frames),
                  'PSX_REPLAY_TEST_INPUT_SEED': '316', 'PSX_REPLAY_EXIT_AT_END': '1'}
    entry = {'mode': 'power_on', 'player_sha256': sha(exe), 'recordings': [], 'playbacks': []}
    ok = True
    replays = []
    for name, card in [('record-1', None), ('record-2', None), ('record-card', cards)]:
        saves = runs / f'saves-{name}'
        saves.mkdir()
        if card:
            shutil.copyfile(card, saves / 'card1.mcd')
        log = runs / f'{name}.log'
        code = run(base(saves), log, timeout=args.timeout, env=player_env(record_env))
        text = log.read_text(errors='replace')
        written = re.findall(r'^replay_recorded: path=(.*) frames=(\d+) ', text, flags=re.M)
        partial = re.findall(r'^replay_partial: path=(.*) frames=(\d+)$', text, flags=re.M)
        rec = {'run': name, 'exit': code, 'partial_copies': [int(f) for _, f in partial]}
        if code != 0 or len(written) != 1:
            rec['result'] = 'record failed'
            entry['recordings'].append(rec)
            return entry, False
        path = Path(written[0][0])
        rec.update({'replay': str(path), 'frames': int(written[0][1]), 'replay_sha256': sha(path),
                    'in_replays_dir': path.parent == saves / 'replays',
                    'name_ok': re.fullmatch(r'.+-boot-\d{8}T\d{6}Z\.psxrpl', path.name) is not None,
                    'partial_left': any(path.parent.glob('*.partial.psxrpl'))})
        ok = ok and rec['frames'] == args.frames and rec['in_replays_dir'] and rec['name_ok'] \
            and not rec['partial_left'] and (args.frames <= 1800 or 1800 in rec['partial_copies'])
        entry['recordings'].append(rec)
        replays.append(path)
    entry['recordings_identical'] = same_replay(replays[0], replays[1])
    entry['card_read_observed'] = end_checkpoint(replays[0]) != end_checkpoint(replays[2])
    ok = ok and entry['recordings_identical']
    flipped = runs / 'replay-start-held.psxrpl'
    hold_start(replays[0], flipped, args.frames // 2, 120)
    for name, path, want in [('play-1', replays[0], 0), ('play-2', replays[0], 0),
                             ('negative-control', flipped, 3)]:
        saves = runs / f'saves-{name}'
        saves.mkdir()
        shutil.copyfile(cards, saves / 'card1.mcd')    # the player's own, different card
        card_before = sha(saves / 'card1.mcd')
        verdict_path = runs / f'{name}.verdict.json'
        log = runs / f'{name}.log'
        code = run(base(saves), log, timeout=args.timeout, env=player_env({
            'PSX_REPLAY_FILE': str(path), 'PSX_REPLAY_EXIT_AT_END': '1',
            'PSX_REPLAY_VERDICT': str(verdict_path)}))
        verdict = json.loads(verdict_path.read_text()) if verdict_path.is_file() else {}
        play = {'run': name, 'exit': code, 'expected': want,
                'verdict': {k: verdict.get(k) for k in ('result', 'frames_played', 'frames_total',
                            'first_divergence_frame', 'power_on', 'recorded_exe_sha256',
                            'player_exe_sha256')},
                'player_card_unchanged': sha(saves / 'card1.mcd') == card_before}
        good = code == want and verdict.get('power_on') is True and play['player_card_unchanged']
        if want == 0:
            good = good and verdict.get('result') == 'in_sync' and verdict.get('frames_played') == args.frames \
                and verdict.get('recorded_exe_sha256') == entry['player_sha256'] \
                and verdict.get('player_exe_sha256') == entry['player_sha256']
        play['pass'] = good
        ok = ok and good
        entry['playbacks'].append(play)
    entry['result'] = 'PASS' if ok else 'FAIL'
    return entry, ok


def must(argv, log: Path, env=None):
    if run(argv, log, env=env) != 0:
        raise SystemExit(f'failed: see {log}')


def build(args, out: Path) -> dict[str, Path]:
    for tool in ('gcc', 'g++', 'cmake', 'ninja'):
        if not shutil.which(tool):
            raise SystemExit(f'{tool} is missing from PATH')
    bios = args.bios.resolve(strict=True)
    tekken3.require_hash(bios, tekken3.BIOS_SHA)
    cue = media_container.resolve_disc(args.disc, cache=out / 'chd-cache', chdman=args.chdman).resolve(strict=True)
    tracks = tekken3.verify_cue(cue)
    project = out / 'project'
    project.mkdir(parents=True)
    boot = project / 'SLUS_004.02'
    boot.write_bytes(tekken3.extract_executable(tracks[0]))
    tekken3.require_hash(boot, tekken3.EXE_SHA)
    staged_bios = project / 'SCPH1001.BIN'
    staged_bios.write_bytes(bios.read_bytes())
    q = lambda p: json.dumps(Path(p).as_posix())
    bios_profile = project / 'bios.toml'
    profile = (ROOT / 'bios/SCPH1001.toml').read_text()
    for key, path in [('rom', staged_bios), ('seeds', ROOT / 'recompiler/seeds/phase2_ghidra_seeds.json'),
                      ('out_dir', ROOT / 'generated')]:
        profile = re.sub(r'^' + key + r'\s*=.*$', lambda _: key + ' = ' + q(path), profile, flags=re.M)
    bios_profile.write_text(profile, encoding='utf8')
    game = project / 'game.toml'
    game.write_text(f'''[game]
name = "Tekken 3 replay determinism"
id = "SLUS-00402"
exe = {q(boot)}
load_address = "0x80010000"
entry_pc = "0x80079C70"
text_size = "0x121000"
stack_base = "0x801FFFF0"
[recompiler]
seeds = {q(TAS / 'tekken3-seeds.txt')}
bios_config = {q(bios_profile)}
strict = true
out_dir = {q(project / 'generated')}
[runtime]
window_title = "Tekken 3 replay determinism"
bios_hle = false
[video]
renderer = "software"
''', encoding='utf8', newline='\n')
    common = ['-G', 'Ninja', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++']
    tools = out / 'tools'
    must(['cmake', '-S', ROOT / 'recompiler', '-B', tools, *common, '-DCMAKE_BUILD_TYPE=Release',
          '-DPSXRECOMP_ENABLE_CHD=ON', '-DBUILD_TESTING=OFF'], out / 'configure-tools.log')
    must(['cmake', '--build', tools, '--parallel', args.jobs], out / 'build-tools.log')
    must([tools / 'psxrecomp-bios.exe', '--config', bios_profile, '--rom', staged_bios,
          '--out-dir', ROOT / 'generated'], out / 'generate-bios.log')
    bash = media_container.find_git_bash()
    fingerprint = subprocess.check_output([str(bash), (ROOT / 'tools/bios_emitter_fingerprint.sh').as_posix(),
                                           bios_profile.as_posix()], cwd=ROOT, text=True, encoding='utf-8', errors='replace').strip()
    (ROOT / 'generated/SCPH1001.emitter.sha').write_text(fingerprint + '\n')
    must([tools / 'psxrecomp-game.exe', '--config', game], out / 'generate-game.log')
    players = {}
    for opt in args.opt:
        native = out / f'native-{opt}'
        flags = '-O0' if opt == 'O0' else '-O2'
        must(['cmake', '-S', TAS, '-B', native, *common, '-DCMAKE_BUILD_TYPE=None',
              f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_CXX_FLAGS={flags}',
              '-DPSX_RECOMP_UI=OFF', '-DPSX_NETPLAY=OFF', '-DPSX_REWIND=OFF', '-DPSX_SETUP_WIZARD=OFF',
              '-DPSX_DEBUG_TOOLS=OFF', '-DPSX_ENABLE_VULKAN=OFF', '-DTAS_PROJECT_DIR=' + str(project),
              '-DTAS_EXE_NAME=Tekken3-Replay', '-DPSXRECOMP_BIOS_PROFILE=' + str(bios_profile),
              '-D_psxrt_bash=' + str(bash), '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE',
              '-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE'], out / f'configure-{opt}.log')
        must(['cmake', '--build', native, '--parallel', args.jobs], out / f'build-{opt}.log')
        players[opt] = native / 'Tekken3-Replay.exe'
    return {'players': players, 'game': game, 'disc': cue, 'bios': staged_bios}


def player_env(extra: dict[str, str]) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PSX_REPLAY_', 'PSX_INPUT_ROUTE'))}
    env.update(extra)
    return env


def flip_one_input(src: Path, dst: Path, frame: int) -> None:
    """Change the buttons of one DualShock record (PSXRTI3, record size 12)."""
    data = bytearray(src.read_bytes())
    ext = int.from_bytes(data[24:28], 'little')
    frames = int.from_bytes(data[16:20], 'little')
    assert data[:8] == b'PSXRTI3\0' and int.from_bytes(data[12:16], 'little') == 12
    frame = min(frame, frames - 1)
    at = 28 + ext + frame * 12 + 4
    data[at] ^= 0x40          # Cross
    dst.write_bytes(bytes(data))


def hold_start(src: Path, dst: Path, frame: int, count: int) -> None:
    """Hold Start for `count` records from `frame` (PSXRTI3, record size 12).
    A power-on replay's negative control: early in a boot a single changed
    press can fall on frames the title ignores, a held Start skips an intro
    or opens a menu."""
    data = bytearray(src.read_bytes())
    ext = int.from_bytes(data[24:28], 'little')
    frames = int.from_bytes(data[16:20], 'little')
    assert data[:8] == b'PSXRTI3\0' and int.from_bytes(data[12:16], 'little') == 12
    for f in range(min(frame, frames - 1), min(frame + count, frames)):
        data[28 + ext + f * 12 + 4] &= ~0x08 & 0xFF    # active low: Start pressed
    dst.write_bytes(bytes(data))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--disc', type=Path, required=True)
    ap.add_argument('--bios', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--chdman', type=Path)
    ap.add_argument('--jobs', default=str(os.cpu_count() or 8))
    ap.add_argument('--opt', nargs='+', default=['O0', 'O2'], choices=['O0', 'O2'])
    ap.add_argument('--record-at', type=int, default=2400, help='vblank where recording starts')
    ap.add_argument('--frames', type=int, default=1800, help='frames recorded')
    ap.add_argument('--timeout', type=int, default=3600)
    ap.add_argument('--reuse-build', action='store_true', help='reuse an existing build in --output')
    ap.add_argument('--power-on', action='store_true',
                    help='check power-on replays (PS1B-316) instead of anchored ones; --record-at is unused')
    args = ap.parse_args()
    out = args.output.resolve()
    if args.reuse_build:
        built = json.loads((out / 'build.json').read_text())
        built = {'players': {k: Path(v) for k, v in built['players'].items()},
                 'game': Path(built['game']), 'disc': Path(built['disc']), 'bios': Path(built['bios'])}
    else:
        out.mkdir(parents=True, exist_ok=False)
        built = build(args, out)
        (out / 'build.json').write_text(json.dumps({'players': {k: str(v) for k, v in built['players'].items()},
            'game': str(built['game']), 'disc': str(built['disc']), 'bios': str(built['bios'])}, indent=2))
    source = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True, encoding='utf-8', errors='replace').strip()
    report = {'schema': 'ps1b-191-replay-determinism-v1', 'source': source,
              'record_at': args.record_at, 'frames': args.frames, 'runs': []}
    if args.power_on:
        report['schema'] = 'ps1b-316-power-on-replay-v1'
        report['record_at'] = 0
    ok = True
    for opt, exe in built['players'].items():
        runs = out / f'runs-{opt}-{time.strftime("%Y%m%d-%H%M%S")}'
        runs.mkdir()
        if args.power_on:
            entry, good = power_on_runs(exe, built, runs, args)
            entry['opt'] = opt
            report['runs'].append(entry)
            ok = ok and good
            continue
        base = [exe, '--headless', '--game', built['game'], '--disc', built['disc'], '--bios', built['bios']]
        replay = runs / 'replay.psxrpl'
        rec = run(base, runs / 'record.log', timeout=args.timeout, env=player_env({
            'PSX_REPLAY_RECORD_FILE': str(replay), 'PSX_REPLAY_RECORD_AT': str(args.record_at),
            'PSX_REPLAY_RECORD_FRAMES': str(args.frames), 'PSX_REPLAY_TEST_INPUT_SEED': '191',
            'PSX_REPLAY_EXIT_AT_END': '1'}))
        entry = {'opt': opt, 'player_sha256': sha(exe), 'record_exit': rec}
        if rec != 0 or not replay.is_file():
            entry['result'] = 'record failed'
            ok = False
            report['runs'].append(entry)
            continue
        entry['replay_sha256'] = sha(replay)
        results = []
        for name, path, want in [('play-1', replay, 0), ('play-2', replay, 0),
                                 ('negative-control', runs / 'replay-flipped.psxrpl', 3)]:
            if name == 'negative-control':
                flip_one_input(replay, path, args.frames // 2)
            log = runs / f'{name}.log'
            code = run(base, log, timeout=args.timeout, env=player_env({
                'PSX_REPLAY_FILE': str(path), 'PSX_REPLAY_EXIT_AT_END': '1'}))
            lines = log.read_text(errors='replace').splitlines()
            end = next((l.strip() for l in lines if l.startswith('replay_end:')), '')
            diverged = next((l.strip() for l in lines if l.startswith('replay_diverged:')), '')
            results.append({'run': name, 'exit': code, 'expected': want, 'replay_end': end,
                            'replay_diverged': diverged})
            ok = ok and code == want
        entry['playbacks'] = results
        report['runs'].append(entry)
    report['result'] = 'PASS' if ok else 'FAIL'
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
