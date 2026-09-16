"""Prepare and compare the original Abe's Oddysee 5620M movie with owned inputs.

Octoshock 2.3 digital-pad title, so the mechanics are Pepsiman's: a PSXRTI v1 route from
bk2_to_psxrti, the Octoshock cold random tape, and the 2.3 CD-DA/MDEC models over the shared
2.2.2 profile. The source reference is the one octoshock2x_admission.py admits, consumed the
way megamanx4.py consumes its own, so admission and replay stay separate tools.

No memory card: the movie's sync settings attach none, unlike Bio Hazard and the Mega Man
titles, so nothing here carries card identity.

The native profile remains experimental until its complete source comparison passes. No
reference is generated from a candidate, and no movie input is changed to correct a divergence.
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

import build_cache
import media_container
import octoshock2x_admission as source
import tekken3
from bk2_to_psxrti import convert
from observation_evidence import compare_returns, terminal_consistency
from replay_prefix import native_span, write_prefix_route, compare_prefix_returns, parse_ladder, run_ladder
from launch_identity import resolve_binary, receipt_fields, add_launch_arguments, check_launch_arguments, run_native_arguments, checkpoint_resume, checkpoint_receipt
from run_native import route_identity
from stream_compare import Watcher, page_reference

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TITLE = 'abesoddysee-5620M'
SPEC = source.TITLES[TITLE]
FRAMES = SPEC['frames']                       # 46,254 original inputs
MOVIE_SHA = SPEC['movie_sha256']
# Pad words produced by bk2_to_psxrti.convert over that movie; the runtime reports the words it
# actually applied, and replay() requires the two to agree.
WORDS_SHA = 'f9aa3f7d888ab02a5cdade99a8cd38486b423d09c2028bcb2722a008effb9a64'
BIOS_STEM = 'SCPH5501'
BIOS_SHA = source.FIRMWARE[SPEC['firmware']][1]
BOOT = 'SLUS_001.90'
GAME_ID = 'SLUS-00190'
EXE_SHA = 'ff2e7977d558f4b6bfc93eb2c1d771adbb25b40344cab48f31722f9c07415fe4'
DISC_STEM = "Oddworld - Abe's Oddysee (USA)"
TRACK_BYTES = 702027312
# Read from the boot program's own PS-X EXE header and cross-checked on every setup, because a
# cloned adapter's pinned entry/text pair is exactly the constant that goes unnoticed until the
# recompiler refuses it (Mega Man X4, 2026-09-16).
LOAD_ADDRESS = 0x80010000
ENTRY_PC = 0x8007F7B0
TEXT_SIZE = 0x93000
STACK_BASE = 0x801FFFF0
SYSTEM_CNF = b'BOOT = cdrom:\\SLUS_001.90;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n'

digest = tekken3.digest
write = tekken3.write_json
command = tekken3.command
require_hash = tekken3.require_hash


def verify_reference(path):
    """The admitted source reference, re-checked against this title's own constants."""
    ref = source.read(path)
    if (ref.get('schema') != SPEC['schema'] or ref.get('source_qualification') != 'pass'
            or ref.get('movie_sha256') != MOVIE_SHA or ref.get('original_inputs') != FRAMES):
        raise ValueError("wrong independent Abe's Oddysee source reference")
    endpoint = ref.get('observed_returns')
    if type(endpoint) is not int or not FRAMES <= endpoint <= FRAMES + 6000 or ref.get('neutral_tail') != endpoint - FRAMES:
        raise ValueError('invalid source reference endpoint')
    bindings = {str(Path(x['path']).resolve()): x['sha256'] for x in ref['bindings']}
    if len(bindings) != len(ref['bindings']):
        raise ValueError('duplicate source reference binding')
    for p, h in bindings.items():
        require_hash(Path(p), h)
    for key in ('ram_pages', 'terminal_ram'):
        if str(Path(ref[key]).resolve()) not in bindings:
            raise ValueError('unbound source reference field')
    if ref.get('admission_tool_sha256') != source.digest(Path(source.__file__)):
        raise ValueError('source reference admission tool differs')
    stock, observed = Path(ref['stock_source']).resolve(), Path(ref['observer_source']).resolve()
    if stock == observed:
        raise ValueError('source reference roles must be independent')
    if (Path(ref['ram_pages']).resolve() != observed / 'ram-pages.tsv'
            or Path(ref['terminal_ram']).resolve() != observed / f'ram-frame-{endpoint:06d}.bin'):
        raise ValueError('source reference fields do not belong to the admitted roles')
    for root, role in ((stock, 'stock'), (observed, 'observer')):
        for name in ('manifest.json', 'complete.json', 'exit.json', 'semantic-review.json'):
            if str(root / name) not in bindings:
                raise ValueError('source reference is missing admission evidence')
        admitted, terminal, _ = source.admit(root, TITLE, role)
        if admitted != endpoint or terminal[3].lower() != ref['terminal_ram_sha256']:
            raise ValueError('source reference terminal differs')
    return ref


def media(cue, bios):
    require_hash(cue, SPEC['media'][DISC_STEM + '.cue'])
    require_hash(bios, BIOS_SHA)
    lines = [x.strip() for x in cue.read_text().splitlines() if x.strip()]
    match = re.fullmatch(r'FILE "([^"\r\n]+)" BINARY', lines[0]) if lines else None
    if not match or lines[1:] != ['TRACK 01 MODE2/2352', 'INDEX 01 00:00:00']:
        raise ValueError('wrong single-track %s topology' % DISC_STEM)
    track = (cue.parent / match[1]).resolve(strict=True)
    if not track.is_relative_to(cue.parent.resolve()) or track.stat().st_size != TRACK_BYTES:
        raise ValueError('invalid %s data track' % DISC_STEM)
    require_hash(track, SPEC['media'][DISC_STEM + '.bin'])
    return track


def boot_program(track):
    sys.path.insert(0, str(ROOT / 'tools'))
    from prepare_disc import parse_root_entries
    with track.open('rb') as f:
        def user(lba):
            f.seek(lba * 2352)
            sector = f.read(2352)
            if len(sector) != 2352 or sector[:12] != b'\0' + b'\xff' * 10 + b'\0' or sector[15] != 2:
                raise ValueError('invalid raw Mode2 data sector')
            return sector[24:2072]

        def span(lba, size):
            if not 0 < size <= 2097152:
                raise ValueError('boot metadata outside bounded size')
            return b''.join(user(lba + i) for i in range((size + 2047) // 2048))[:size]
        pvd = user(16)
        if pvd[1:6] != b'CD001':
            raise ValueError('missing ISO9660')
        tree = parse_root_entries(span(struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]))
        if span(*tree['SYSTEM.CNF']) != SYSTEM_CNF:
            raise ValueError('wrong boot configuration')
        data = span(*tree[BOOT])
    if hashlib.sha256(data).hexdigest() != EXE_SHA:
        raise ValueError("wrong Abe's Oddysee executable")
    if data[:8] != b'PS-X EXE':
        raise ValueError('boot program is not a PS-X EXE')
    pc0, _, t_addr, t_size = struct.unpack_from('<IIII', data, 0x10)
    stack = struct.unpack_from('<I', data, 0x30)[0]
    if (pc0, t_addr, t_size, stack) != (ENTRY_PC, LOAD_ADDRESS, TEXT_SIZE, STACK_BASE):
        raise ValueError('boot program header differs from the pinned game profile: '
                         'pc0=%08X load=%08X text=%X stack=%08X' % (pc0, t_addr, t_size, stack))
    return data


def setup(args):
    if os.name != 'nt':
        raise ValueError('Windows UCRT build required for this candidate')
    if subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip():
        raise ValueError('commit source changes before building')
    build_head = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    if not 1 <= args.jobs <= 64:
        raise ValueError('jobs must be in 1..64')
    for tool in ['gcc', 'g++', 'cmake', 'ninja', 'git']:
        if not shutil.which(tool):
            raise ValueError('missing tool: ' + tool)
    bash = media_container.find_git_bash()
    macros = subprocess.check_output(['gcc', '-dM', '-E', '-include', '_mingw.h', '-'], input='', text=True)
    if not re.search(r'^#define _UCRT\b', macros, re.M):
        raise ValueError('UCRT compiler required')
    reference_path = args.reference.resolve(strict=True)
    reference = verify_reference(reference_path)
    container = media_container.resolve_disc(args.disc, cache=args.chd_cache, chdman=args.chdman, stem=DISC_STEM)
    disc, bios, movie = [p.resolve(strict=True) for p in (container, args.bios, args.movie)]
    track = media(disc, bios)
    require_hash(movie, MOVIE_SHA)
    project = args.project.resolve()
    if project == ROOT or project.is_relative_to(ROOT):
        raise ValueError('put generated title/build output outside source')
    project.mkdir(parents=True, exist_ok=False)
    data = boot_program(track)
    exe = project / BOOT
    exe.write_bytes(data)
    staged_bios = project / (BIOS_STEM + '.BIN')
    shutil.copyfile(bios, staged_bios)
    require_hash(staged_bios, BIOS_SHA)
    tape = project / 'octoshock-cold-random.psxrng'
    command([sys.executable, HERE / 'external/source_random_tape.py', tape], project / 'random-tape.log')
    require_hash(tape, tekken3.TAPE_SHA)
    payload, receipt = convert(movie.read_bytes())
    if receipt['frame_count'] != FRAMES or receipt['pad_words_le_sha256'] != WORDS_SHA:
        raise ValueError('original controller words differ')
    route = project / 'input.psxrti'
    route.write_bytes(payload)
    write(project / 'input.json', receipt)
    # Build cache (docs/tasreplays/build-cache.md): each stage is reused only on an exact
    # content-key match and recorded in setup.json; every check below runs on every path.
    cache_root = build_cache.resolve_root(args)
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++']
    tools_argv = lambda tools: ['cmake', '-S', ROOT / 'recompiler', '-B', tools, *common, '-DBUILD_TESTING=ON',
                                '-DPSXRECOMP_ENABLE_CHD=ON', '-DPython3_EXECUTABLE=' + sys.executable]

    def build_tools(tools):
        command(tools_argv(tools), project / 'configure-tools.log', env=build_cache.build_env())
        command(['cmake', '--build', tools, '--parallel', str(args.jobs)], project / 'build-tools.log', env=build_cache.build_env())
        command(['ctest', '--test-dir', tools, '--output-on-failure', '-j', str(args.jobs)], project / 'test-tools.log', env=build_cache.build_env())
    tools, tools_record = build_cache.stage_tools(cache_root, ROOT, project, args.tools_dir.resolve() if args.tools_dir else None,
                                                  lambda: build_cache.tools_inputs(ROOT, tools_argv(project / 'tools')), build_tools, build_head)
    q = lambda p: json.dumps(p.as_posix())
    bios_profile = project / 'bios.toml'
    profile = (ROOT / ('bios/' + BIOS_STEM + '.toml')).read_text()
    for key, value in [('rom', staged_bios), ('seeds', ROOT / 'recompiler/seeds/phase2_ghidra_seeds.json'), ('out_dir', ROOT / 'generated')]:
        profile = re.sub(r'^' + key + r'\s*=.*$', lambda _, v=value: key + ' = ' + q(v), profile, flags=re.M)
    bios_profile.write_text(profile, encoding='utf8')
    game = project / 'game.toml'
    game.write_text(f'''[game]
name = "Abe's Oddysee TAS"
id = "{GAME_ID}"
exe = {q(exe)}
load_address = "0x{LOAD_ADDRESS:08X}"
entry_pc = "0x{ENTRY_PC:08X}"
text_size = "0x{TEXT_SIZE:X}"
stack_base = "0x{STACK_BASE:08X}"
[recompiler]
seeds = {q(project / 'seeds.txt')}
bios_config = {q(bios_profile)}
strict = true
out_dir = {q(project / 'generated')}
[runtime]
window_title = "Abe's Oddysee - original TAS"
bios_hle = false
[video]
renderer = "software"
''', encoding='utf8')

    def stamp():
        fingerprint = subprocess.check_output([str(bash), (ROOT / 'tools/bios_emitter_fingerprint.sh').as_posix(),
                                               bios_profile.as_posix()], cwd=ROOT, text=True).strip()
        if not re.fullmatch('[0-9a-f]{64}', fingerprint):
            raise ValueError('invalid generated BIOS fingerprint')
        (ROOT / ('generated/' + BIOS_STEM + '.emitter.sha')).write_text(fingerprint + '\n')
        return fingerprint
    stamped = []

    def generate():
        command([tools / 'psxrecomp-toml.exe', exe, '--output', project / 'census.toml', '--seeds', project / 'seeds.txt'], project / 'census.log')
        command([tools / 'psxrecomp-bios.exe', '--config', bios_profile, '--rom', staged_bios,
                 '--out-dir', ROOT / 'generated'], project / 'generate-bios.log')
        stamped.append(stamp())
        command([tools / 'psxrecomp-game.exe', '--config', game], project / 'generate-game.log')
    generated_record = build_cache.stage_generated(cache_root, ROOT, project, BIOS_STEM,
        lambda: build_cache.generated_inputs(tools, BIOS_STEM, staged_bios, ROOT / 'recompiler/seeds/phase2_ghidra_seeds.json',
                                             build_cache.blob_id(ROOT, 'bios/' + BIOS_STEM + '.toml'),
                                             build_cache.portable_emitter_fingerprint(bash, ROOT, bios_profile), exe, game),
        True, generate, build_head)
    # On a cache hit the emitter fingerprint is recomputed and written exactly as on a miss.
    fingerprint = stamped[0] if stamped else stamp()
    native = project / 'native'
    native_argv = ['cmake', '-S', HERE, '-B', native, *common, '-DTAS_PROJECT_DIR=' + str(project),
                   '-DTAS_GAME_STEM=' + BOOT, '-DTAS_EXE_NAME=AbesOddysee-TAS', '-DTAS_WINDOW_TITLE=Abes Oddysee TAS',
                   '-DPSXRECOMP_BIOS_STEMS=' + BIOS_STEM, '-DPSX_SHELLWIN_INTERP=ON',
                   '-DPSXRECOMP_BIOS_PROFILE=' + str(bios_profile), '-D_psxrt_bash=' + str(bash),
                   '-DPSX_RECOMP_UI=OFF', '-DPSX_NETPLAY=OFF', '-DPSX_REWIND=OFF', '-DPSX_SETUP_WIZARD=OFF',
                   '-DPSX_DEBUG_TOOLS=ON', '-DPSX_ENABLE_VULKAN=OFF',
                   '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE', '-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE']

    def build_native():
        command(native_argv, project / 'configure-native.log', env=build_cache.build_env())
        command(['cmake', '--build', native, '--parallel', str(args.jobs)], project / 'build-native.log', env=build_cache.build_env())
    native_record = build_cache.stage_native(cache_root, ROOT, project, native, 'AbesOddysee-TAS',
        lambda: build_cache.native_inputs(ROOT, build_cache.generated_set(ROOT, BIOS_STEM, project), native_argv, bios_profile),
        build_native, build_head)
    if (subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip()
            or subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip() != build_head):
        raise ValueError('source changed during candidate build')
    files = [disc, track, bios, staged_bios, bios_profile, movie, exe, game, tape, route,
             project / 'seeds.txt', reference_path]
    build = native / 'AbesOddysee-TAS.exe'
    generated = {str(f.relative_to(ROOT)): digest(f) for f in (ROOT / 'generated').glob(BIOS_STEM + '*') if f.is_file()}
    generated.update({str(f): digest(f) for f in (project / 'generated').glob('*') if f.is_file()})
    info = {'schema': 'abesoddysee-tas-candidate-v1', 'source_head': build_head,
            'source_tree': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD^{tree}'], text=True).strip(),
            'reference': str(reference_path), 'bindings': [{'path': str(f), 'sha256': digest(f)} for f in files],
            'generated': generated, 'executable': str(build), 'executable_sha256': digest(build),
            'disc': str(disc), 'bios': str(staged_bios), 'game': str(game), 'route': str(route), 'tape': str(tape),
            'bios_emitter_fingerprint': fingerprint,
            'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
            'tools_build_dir': str(tools),
            'build_cache': build_cache.receipt_block(cache_root, tools_record, generated_record, native_record),
            'qualification': 'candidate only; source/native comparison pending'}
    write(project / 'setup.json', info)
    print(json.dumps({'candidate': str(build), 'sha256': digest(build)}))


def replay(args, returns=None, output=None):
    """One replay: full when returns is None or the endpoint, otherwise a diagnostic prefix."""
    output = args.output if output is None else Path(output)
    info = json.loads((args.project / 'setup.json').read_text())
    if info.get('schema') != 'abesoddysee-tas-candidate-v1':
        raise ValueError("wrong Abe's Oddysee candidate")
    for binding in info['bindings']:
        require_hash(Path(binding['path']), binding['sha256'])
    if args.exe is None and args.diagnostic_binary is None:
        require_hash(Path(info['executable']), info['executable_sha256'])
    binary = resolve_binary(info['executable'], info['executable_sha256'], args.exe, args.diagnostic_binary)
    reference = None
    observation_end = FRAMES
    if args.reference:
        reference = verify_reference(args.reference.resolve(strict=True))
        if str(args.reference.resolve()) != info['reference']:
            raise ValueError('candidate was built against a different source reference')
        observation_end = reference['observed_returns']
    wanted = observation_end if returns is None else returns
    # A resumed run compares only the returns after its checkpoint and never qualifies.
    start, consumed = checkpoint_resume(args)
    if start >= wanted:
        raise ValueError('the checkpoint is not before the compared endpoint')
    records, tail = native_span(FRAMES, observation_end, wanted)
    prefix = wanted < observation_end
    route = Path(info['route'])
    words_sha = WORDS_SHA
    if prefix:
        output.parent.mkdir(parents=True, exist_ok=True)
        route = write_prefix_route(route, records, output.with_name(output.name + '-input.psxrti'))
        words_sha = route_identity(route)['words_sha256']
    if start:
        # The runtime hashes only the inputs it delivers after the checkpoint.
        words_sha = route_identity(route, consumed)['words_sha256']
    argv = [sys.executable, HERE / 'run_native.py', output, '--exe', binary['path'],
            '--game', info['game'], '--disc', info['disc'], '--bios', info['bios'], '--route', route,
            '--cd-source-clock-tape', info['tape'], '--neutral-tail', str(tail), '--timeout', str(args.timeout),
            '--storage-budget-mib', '1536', '--cd-cdda-model', 'octoshock-2.3', '--mdec-source-model', 'octoshock-2.3',
            '--checkpoint-every', '1200', '--renderer', 'software', *run_native_arguments(args, binary, wanted), *tekken3.PROFILE]
    if args.show:
        argv.append('--show')
    if reference:
        argv += ['--ram-snapshot-frame', str(wanted)]
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise ValueError('choose a fresh native output directory')
    log = output.parent / (output.name + '-launch.log')
    # Streaming evidence only: it may stop the process early, never decide a pass.
    watcher = None
    if reference:
        watcher = Watcher(output, page_reference(Path(reference['ram_pages']), wanted, start), wanted,
                          stop_on_divergence=args.stop_on_divergence, first_frame=start + 1)
        watcher.start()
    try:
        with log.open('x') as stream:
            process = subprocess.run([str(v) for v in argv], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    finally:
        streaming = watcher.finish() if watcher else None
    report = {'status': 'incomplete', 'original_inputs': FRAMES, 'compared_returns': 0,
              'end_frame': wanted + 1, 'source_observation_end': observation_end, 'observed_returns': wanted,
              'diagnostic_prefix': prefix, 'full_original_input_and_tail': not prefix,
              'native_runner_exit': process.returncode, 'first_divergence': None, 'mechanical_match': False,
              **receipt_fields(binary), 'streaming': streaming, 'checkpoints': checkpoint_receipt(args, wanted),
              'scope': ('diagnostic prefix of unchanged original inputs; no ending or terminal RAM claim' if prefix else
                        'unchanged original inputs and declared neutral ending; full RAM/clock source compatibility')}
    if reference:
        try:
            complete = None
            if prefix:
                comparison = compare_prefix_returns(Path(reference['ram_pages']), output / 'ram-pages.tsv', wanted, start)
                report.update({k: v for k, v in comparison.items() if k != 'match'})
                complete = json.loads((output / 'complete.json').read_text())
                report['input_identity_matches'] = (complete['input_frames'] == records and complete['applied_words_sha256'] == words_sha
                                                    and complete['frame'] == wanted + 1 and complete['neutral_tail_ticks'] == 0)
                okay = process.returncode == 0 and report['input_identity_matches'] and comparison['match']
            else:
                comparison = compare_returns(Path(reference['ram_pages']), output / 'ram-pages.tsv', observation_end, start)
                report.update({k: v for k, v in comparison.items() if k != 'match'})
                complete = json.loads((output / 'complete.json').read_text())
                report['input_identity_matches'] = (complete['input_frames'] == FRAMES and complete['applied_words_sha256'] == words_sha
                                                    and complete['frame'] == observation_end + 1
                                                    and complete['neutral_tail_ticks'] == observation_end - FRAMES + 1)
                raw = terminal_consistency(Path(reference['ram_pages']), Path(reference['terminal_ram']), observation_end)
                actual = terminal_consistency(output / 'ram-pages.tsv', output / f'ram-frame-{observation_end:06d}.bin', observation_end,
                                              first_frame=start + 1)
                report['terminal_ram_matches'] = actual == raw
                okay = (process.returncode == 0 and report['input_identity_matches']
                        and report['terminal_ram_matches'] and comparison['match'])
            report['mechanical_match'] = bool(okay)
            report['status'] = 'pass' if okay else 'fail'
        except (ValueError, OSError, KeyError) as error:
            report.update(status='fail', error=str(error))
    elif process.returncode:
        report['status'] = 'fail'
    if report['status'] == 'pass':
        # A non-setup binary or a prefix never claims the qualifying full pass.
        if not binary['binary_matches_setup'] or start:
            report['status'] = 'diagnostic'
        elif prefix:
            report['status'] = 'prefix_pass'
    if (output / 'exit.json').exists():
        host = json.loads((output / 'exit.json').read_text())
        if host.get('timed_out') or host.get('stop_reason') in {'host_timeout', 'host_storage_budget'}:
            report.update(status='incomplete', host_limit=host.get('stop_reason') or 'host_timeout',
                          classification='host resource limit; no guest failure inferred')
    write(output / 'verification.json', report)
    print(json.dumps(report))
    return report


def run(args):
    check_launch_arguments(args)
    if args.ladder is not None:
        if not args.reference:
            raise ValueError('--ladder requires --reference')
        observation_end = json.loads(args.reference.read_text()).get('observed_returns')
        if type(observation_end) is not int:
            raise ValueError('invalid reference boundary')
        report = run_ladder(args.output, parse_ladder(args.ladder, observation_end), observation_end,
                            lambda returns, target: replay(args, returns, target), write)
        return 0 if report['status'] in ('pass', 'prefix_pass') else 1
    report = replay(args, args.returns)
    return 0 if report['status'] in ('pass', 'prefix_pass') else 2 if report['status'] == 'incomplete' else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    s = sub.add_parser('setup')
    for field in ['disc', 'bios', 'movie', 'project', 'reference']:
        s.add_argument('--' + field, type=Path, required=True)
    s.add_argument('--jobs', type=int, default=4)
    s.add_argument('--chdman', type=Path, help='chdman.exe for .chd input; otherwise PSX_CHDMAN or PATH')
    s.add_argument('--chd-cache', type=Path, help='private directory for split .chd tracks, keyed by container hash')
    s.add_argument('--tools-dir', type=Path, help='Reuse a CMake tool build for this source; reconfigure, rebuild and rerun all tests')
    build_cache.add_arguments(s)
    r = sub.add_parser('run')
    r.add_argument('--project', type=Path, required=True)
    r.add_argument('--output', type=Path, required=True)
    r.add_argument('--timeout', type=int, default=43200)
    r.add_argument('--show', action='store_true')
    r.add_argument('--reference', type=Path)
    add_launch_arguments(r)
    args = p.parse_args()
    return {'setup': setup, 'run': run}[args.action](args)


if __name__ == '__main__':
    raise SystemExit(main())
