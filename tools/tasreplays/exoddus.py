"""Prepare and compare the original Abe's Exoddus (USA) 100% 6672M movie with owned inputs.

The movie was recorded on BizHawk 2.10 with a digital pad on each port and swaps discs once:
the tray opens at frame 179,993, disc 2 is selected and the tray closes at 179,996. P2 is
never pressed, but it is connected, so the route keeps both ports (octoshock210_route writes
a PSXRTI3 two-digital-pad route with those three console events and the two-disc set).

The device models are Crash Bandicoot's (crash.py, BizHawk 2.7): BizHawk 2.10 carries the same
Octoshock, and octoshock2x_admission.py admits both hosts with the same lua. Two things differ:
the pads are digital, so the pad ACK model is a run option measured on prefixes rather than a
fixed profile entry (Nymashock 1.29.0 has a DualShock ACK model only), and the build is made
from disc 1's program. Disc 2's program differs from it in one byte (RAM 0x800630C8, the disc
number the game stores), so it is a continuation of the same program and disc 1's build plays
the whole route.

No memory card: the movie's sync settings attach none. The source reference is the one
octoshock2x_admission.py admits. No reference is generated from a candidate, and no movie input
is changed to correct a divergence.
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
import octoshock2x_admission as source
import octoshock210_route
import tekken3
from observation_evidence import terminal_consistency
from replay_prefix import native_span, write_prefix_route, parse_ladder, run_ladder
from launch_identity import resolve_binary, receipt_fields, add_launch_arguments, check_launch_arguments, run_native_arguments, checkpoint_resume, checkpoint_receipt
from run_native import checkpoint_asset_digest, route_identity
from stream_compare import Watcher, page_reference

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TITLE = 'exoddus-6672M'
SPEC = source.TITLES[TITLE]
FRAMES = SPEC['frames']                       # 478,752 original inputs
MOVIE_SHA = SPEC['movie_sha256']
# octoshock210_route.convert over that movie: the route file and the P1/P2 words it carries.
ROUTE_SHA = 'd48513ae0ce4e7bd04dc9e72f0467c16f644e16a60d16753e94662c7f816d85a'
P1_WORDS_SHA = '82b7582f080926453938de332416b2472e184599ab8c633d5fecc51331147873'
P2_WORDS_SHA = '509fdb9a8ad9145e9aa0cc7a9b22278dc72eeedd06cddb7c88eca94e13898927'
EVENTS = [{'frame': 179993, 'event': 'TRAY_OPEN', 'disc_index': 0},
          {'frame': 179996, 'event': 'DISC_SELECT', 'disc_index': 2},
          {'frame': 179996, 'event': 'TRAY_CLOSE', 'disc_index': 2}]
BIOS_STEM = 'SCPH5501'
BIOS_SHA = source.FIRMWARE[SPEC['firmware']][1]
BOOT = 'SLUS_007.10'
GAME_ID = 'SLUS-00710'
EXE_SHA = '056fa1d3dd618c70f75088626e0362d59bc89078dcec70e0e7ad42af096472bc'
DISC2_BOOT = 'SLUS_007.31'
DISC2_EXE_SHA = 'cd946820652d21a2c5caeaa7d5834520e5d051c7e0a5c077d45f95e1d96b5ff8'
# The full route draws more than the 65,536 words of the qualified cold tape: run full-f1 stopped at
# return 121,935 with the tape spent (PS1G-43). The 1,048,576-word tape extends that prefix exactly;
# Mega Man X4 pins the same file.
TAPE_WORDS = 1048576
TAPE_SHA = '8cb70d09a341acdbf7bd8dd3efc54e8640f988ed9e07d55733c85e04eb4c8ce4'
SET_STEM = "Oddworld - Abe's Exoddus (USA)"
DISC_STEMS = [f'{SET_STEM} (Disc 1)', f'{SET_STEM} (Disc 2)']
TRACK_BYTES = [611978640, 483947520]
# Read from the boot program's own PS-X EXE header and cross-checked on every setup.
LOAD_ADDRESS = 0x80010000
ENTRY_PC = 0x8006EA30
TEXT_SIZE = 0x90000
STACK_BASE = 0x801FFFF0
SYSTEM_CNF = {BOOT: b'BOOT = cdrom:\\SLUS_007.10;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n',
              DISC2_BOOT: b'BOOT = cdrom:\\SLUS_007.31;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n'}
# crash.py's profile without its DualShock ACK model; the digital ACK model is --pad-ack-model.
PROFILE = [
    '--critical-section-model', 'exception', '--syscall-model', 'guest-exception',
    '--field-model', 'octoshock-2.2.2-ntsc-raster',
    '--dma-model', 'octoshock-2.2.2-otc', '--legacy-card-repair', 'off',
    '--cd-firmware-model', 'octoshock-2.2.2', '--cd-cold-status-model', 'octoshock-2.2.2',
    '--cd-toc-seek-model', 'octoshock-2.2.2', '--cd-explicit-seek-model', 'octoshock-2.2.2',
    '--cd-read-start-model', 'octoshock-2.2.2-pipeline', '--cd-dma-model', 'octoshock-2.2.2',
    '--cd-drive-model', 'octoshock-2.7',
    '--cd-cdda-model', 'octoshock-2.3', '--mdec-source-model', 'nymashock-1.29.0',
    '--gpu-status-model', 'octoshock-2.2.2-raster', '--gpu-dma-model', 'octoshock-2.2.2-bounded-quad',
    '--timer1-model', 'octoshock-2.2.2', '--timer2-model', 'octoshock-2.2.2',
    '--precise-slice', 'on', '--cpu-return-probe', '--ram-page-probe',
]
PAD_ACK_MODELS = ('octoshock-2.2.2-digital', 'default')

digest = tekken3.digest
write = tekken3.write_json
command = tekken3.command
require_hash = tekken3.require_hash


def verify_reference(path):
    """The admitted source reference, re-checked against this title's own constants."""
    ref = source.read(path)
    if (ref.get('schema') != SPEC['schema'] or ref.get('source_qualification') != 'pass'
            or ref.get('movie_sha256') != MOVIE_SHA or ref.get('original_inputs') != FRAMES):
        raise ValueError("wrong independent Abe's Exoddus source reference")
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


def media(cue, stem, track_bytes):
    """One disc: the admitted cue and its single raw Mode2 track."""
    require_hash(cue, SPEC['media'][stem + '.cue'])
    lines = [x.strip() for x in cue.read_text(encoding='utf-8').splitlines() if x.strip()]
    match = re.fullmatch(r'FILE "([^"\r\n]+)" BINARY', lines[0]) if lines else None
    if not match or lines[1:] != ['TRACK 01 MODE2/2352', 'INDEX 01 00:00:00']:
        raise ValueError('wrong single-track %s topology' % stem)
    track = (cue.parent / match[1]).resolve(strict=True)
    if not track.is_relative_to(cue.parent.resolve()) or track.stat().st_size != track_bytes:
        raise ValueError('invalid %s data track' % stem)
    require_hash(track, SPEC['media'][stem + '.bin'])
    return track


def boot_program(track, boot, exe_sha):
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
        if span(*tree['SYSTEM.CNF']) != SYSTEM_CNF[boot]:
            raise ValueError('wrong boot configuration')
        data = span(*tree[boot])
    if hashlib.sha256(data).hexdigest() != exe_sha:
        raise ValueError("wrong Abe's Exoddus executable: " + boot)
    if data[:8] != b'PS-X EXE':
        raise ValueError('boot program is not a PS-X EXE')
    pc0, _, t_addr, t_size = struct.unpack_from('<IIII', data, 0x10)
    stack = struct.unpack_from('<I', data, 0x30)[0]
    if (pc0, t_addr, t_size, stack) != (ENTRY_PC, LOAD_ADDRESS, TEXT_SIZE, STACK_BASE):
        raise ValueError('boot program header differs from the pinned game profile: '
                         'pc0=%08X load=%08X text=%X stack=%08X' % (pc0, t_addr, t_size, stack))
    return data


def continuation(disc1, disc2):
    """Disc 2's program is disc 1's with the stored disc number 1 -> 2 (one byte)."""
    diff = [i for i, (a, b) in enumerate(zip(disc1, disc2)) if a != b]
    if len(disc1) != len(disc2) or diff != [0x538C8] or (disc1[0x538C8], disc2[0x538C8]) != (1, 2):
        raise ValueError('disc 2 program is not the one-byte continuation of disc 1')
    return {'file_offset': '0x538C8', 'ram': '0x%08X' % (LOAD_ADDRESS + 0x538C8 - 0x800), 'disc1': 1, 'disc2': 2}


def setup(args):
    if os.name != 'nt':
        raise ValueError('Windows UCRT build required for this candidate')
    if subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True, encoding='utf-8', errors='replace').strip():
        raise ValueError('commit source changes before building')
    build_head = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True, encoding='utf-8', errors='replace').strip()
    if not 1 <= args.jobs <= 64:
        raise ValueError('jobs must be in 1..64')
    for tool in ['gcc', 'g++', 'cmake', 'ninja', 'git']:
        if not shutil.which(tool):
            raise ValueError('missing tool: ' + tool)
    import media_container
    bash = media_container.find_git_bash()
    macros = subprocess.check_output(['gcc', '-dM', '-E', '-include', '_mingw.h', '-'], input='', text=True, encoding='utf-8', errors='replace')
    if not re.search(r'^#define _UCRT\b', macros, re.M):
        raise ValueError('UCRT compiler required')
    reference_path = args.reference.resolve(strict=True)
    verify_reference(reference_path)
    discs = [media_container.resolve_disc(d, cache=args.chd_cache, chdman=args.chdman, stem=s).resolve(strict=True)
             for d, s in zip((args.disc1, args.disc2), DISC_STEMS)]
    bios, movie = args.bios.resolve(strict=True), args.movie.resolve(strict=True)
    require_hash(bios, BIOS_SHA)
    tracks = [media(c, s, n) for c, s, n in zip(discs, DISC_STEMS, TRACK_BYTES)]
    require_hash(movie, MOVIE_SHA)
    project = args.project.resolve()
    if project == ROOT or project.is_relative_to(ROOT):
        raise ValueError('put generated title/build output outside source')
    project.mkdir(parents=True, exist_ok=False)
    data = boot_program(tracks[0], BOOT, EXE_SHA)
    swap = continuation(data, boot_program(tracks[1], DISC2_BOOT, DISC2_EXE_SHA))
    exe = project / BOOT
    exe.write_bytes(data)
    staged_bios = project / (BIOS_STEM + '.BIN')
    shutil.copyfile(bios, staged_bios)
    require_hash(staged_bios, BIOS_SHA)
    tape = project / 'cold-random.psxrng'
    command([sys.executable, HERE / 'external/source_random_tape.py', tape, '--count', TAPE_WORDS], project / 'random-tape.log')
    require_hash(tape, TAPE_SHA)
    payload, receipt = octoshock210_route.convert(movie.read_bytes(), [checkpoint_asset_digest(c) for c in discs])
    if (receipt['frame_count'] != FRAMES or receipt['output_sha256'] != ROUTE_SHA
            or receipt['p1_words_le_sha256'] != P1_WORDS_SHA or receipt['p2_words_le_sha256'] != P2_WORDS_SHA
            or receipt['console_events'] != EVENTS):
        raise ValueError('original controller words or console events differ')
    route = project / 'input.psxrti3'
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
    profile = (ROOT / ('bios/' + BIOS_STEM + '.toml')).read_text(encoding='utf-8')
    for key, value in [('rom', staged_bios), ('seeds', ROOT / 'recompiler/seeds/phase2_ghidra_seeds.json'), ('out_dir', ROOT / 'generated')]:
        profile = re.sub(r'^' + key + r'\s*=.*$', lambda _, v=value: key + ' = ' + q(v), profile, flags=re.M)
    bios_profile.write_text(profile, encoding='utf8')
    # [game] discs is the roster the runtime binds the route's disc set to (run_native --roster-disc).
    game = project / 'game.toml'
    game.write_text(f'''[game]
name = "Abe's Exoddus TAS"
id = "{GAME_ID}"
exe = {q(exe)}
load_address = "0x{LOAD_ADDRESS:08X}"
entry_pc = "0x{ENTRY_PC:08X}"
text_size = "0x{TEXT_SIZE:X}"
stack_base = "0x{STACK_BASE:08X}"
discs = [{q(discs[0])}, {q(discs[1])}]
players = 2
[recompiler]
seeds = {q(project / 'seeds.txt')}
bios_config = {q(bios_profile)}
strict = true
out_dir = {q(project / 'generated')}
[runtime]
window_title = "Abe's Exoddus - original TAS"
bios_hle = false
[video]
renderer = "software"
''', encoding='utf8')

    def stamp():
        fingerprint = subprocess.check_output([str(bash), (ROOT / 'tools/bios_emitter_fingerprint.sh').as_posix(),
                                               bios_profile.as_posix()], cwd=ROOT, text=True, encoding='utf-8', errors='replace').strip()
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
                   '-DTAS_GAME_STEM=' + BOOT, '-DTAS_EXE_NAME=Exoddus-TAS', '-DTAS_WINDOW_TITLE=Abes Exoddus TAS',
                   '-DPSXRECOMP_BIOS_STEMS=' + BIOS_STEM, '-DPSX_SHELLWIN_INTERP=ON',
                   '-DPSXRECOMP_BIOS_PROFILE=' + str(bios_profile), '-D_psxrt_bash=' + str(bash),
                   '-DPSX_RECOMP_UI=OFF', '-DPSX_NETPLAY=OFF', '-DPSX_REWIND=OFF', '-DPSX_SETUP_WIZARD=OFF',
                   '-DPSX_DEBUG_TOOLS=ON', '-DPSX_ENABLE_VULKAN=OFF',
                   '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE', '-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE']

    def build_native():
        command(native_argv, project / 'configure-native.log', env=build_cache.build_env())
        command(['cmake', '--build', native, '--parallel', str(args.jobs)], project / 'build-native.log', env=build_cache.build_env())
    native_record = build_cache.stage_native(cache_root, ROOT, project, native, 'Exoddus-TAS',
        lambda: build_cache.native_inputs(ROOT, build_cache.generated_set(ROOT, BIOS_STEM, project), native_argv, bios_profile),
        build_native, build_head)
    if (subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True, encoding='utf-8', errors='replace').strip()
            or subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True, encoding='utf-8', errors='replace').strip() != build_head):
        raise ValueError('source changed during candidate build')
    files = [*discs, *tracks, bios, staged_bios, bios_profile, movie, exe, game, tape, route,
             project / 'seeds.txt', reference_path]
    build = native / 'Exoddus-TAS.exe'
    generated = {str(f.relative_to(ROOT)): digest(f) for f in (ROOT / 'generated').glob(BIOS_STEM + '*') if f.is_file()}
    generated.update({str(f): digest(f) for f in (project / 'generated').glob('*') if f.is_file()})
    info = {'schema': 'exoddus-tas-candidate-v1', 'source_head': build_head,
            'source_tree': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD^{tree}'], text=True, encoding='utf-8', errors='replace').strip(),
            'reference': str(reference_path), 'bindings': [{'path': str(f), 'sha256': digest(f)} for f in files],
            'generated': generated, 'executable': str(build), 'executable_sha256': digest(build),
            'discs': [str(d) for d in discs], 'bios': str(staged_bios), 'game': str(game), 'route': str(route),
            'tape': str(tape), 'disc2_continuation': swap,
            'profile': PROFILE, 'bios_emitter_fingerprint': fingerprint,
            'compiler': subprocess.check_output(['gcc', '--version'], text=True, encoding='utf-8', errors='replace').splitlines()[0],
            'tools_build_dir': str(tools),
            'build_cache': build_cache.receipt_block(cache_root, tools_record, generated_record, native_record),
            'qualification': 'candidate only; source/native comparison pending'}
    write(project / 'setup.json', info)
    print(json.dumps({'candidate': str(build), 'sha256': digest(build)}))


def replay(args, returns=None, output=None):
    """One replay: full when returns is None or the endpoint, otherwise a diagnostic prefix."""
    output = (args.output if output is None else Path(output)).resolve()
    info = json.loads(args.setup.resolve(strict=True).read_text(encoding='utf-8'))
    if info.get('schema') != 'exoddus-tas-candidate-v1':
        raise ValueError("wrong Abe's Exoddus candidate")
    for binding in info['bindings']:
        require_hash(Path(binding['path']), binding['sha256'])
    if args.exe is None and args.diagnostic_binary is None:
        require_hash(Path(info['executable']), info['executable_sha256'])
    binary = resolve_binary(info['executable'], info['executable_sha256'], args.exe, args.diagnostic_binary)
    if info['profile'] != PROFILE:
        raise ValueError('candidate profile differs; build a new candidate')
    if getattr(args, 'resume_from', None) or getattr(args, 'save_state_at', None) or getattr(args, 'save_state_every', None):
        raise ValueError('the two-pad disc-event route admits no checkpoint resume yet')
    reference = verify_reference(Path(info['reference']))
    endpoint = reference['observed_returns']
    if output.exists():
        raise ValueError('fresh output required')
    output.parent.mkdir(parents=True, exist_ok=True)
    wanted = endpoint if returns is None else returns
    start = checkpoint_resume(args)[0]
    records, tail = native_span(FRAMES, endpoint, wanted)
    route = Path(info['route'])
    if wanted < endpoint:
        route = write_prefix_route(route, records, output.with_name(output.name + '-input.psxrti3'))
    identity = route_identity(route)
    argv = [sys.executable, str(HERE / 'run_native.py'), str(output), '--exe', binary['path'], '--game', info['game'],
            '--route', str(route), '--disc', info['discs'][0], '--bios', info['bios'],
            *[a for d in info['discs'] for a in ('--roster-disc', d)],
            '--cd-source-clock-tape', info['tape'], '--neutral-tail', str(tail), '--timeout', str(args.timeout),
            '--checkpoint-every', '1200', '--renderer', 'software', '--storage-budget-mib', '6144',
            '--pad-ack-model', args.pad_ack_model,
            '--ram-snapshot-frame', str(wanted), *run_native_arguments(args, binary, wanted), *PROFILE]
    # Streaming evidence only: it may stop the process early, never decide a pass.
    watcher = Watcher(output, page_reference(Path(reference['ram_pages']), wanted, start), wanted,
                      stop_on_divergence=args.stop_on_divergence, first_frame=start + 1)
    watcher.start()
    try:
        result = subprocess.run(argv)
    finally:
        streaming = watcher.finish()
    if not output.is_dir():
        raise RuntimeError('native launcher rejected before creating its evidence directory')
    comparison = None
    if (output / 'ram-pages.tsv').exists():
        from compare_ram_pages import read_pages
        from itertools import islice, zip_longest
        first = None; counts = [0, 0]
        try:
            for left, right in zip_longest(islice(read_pages(Path(reference['ram_pages'])), start, wanted),
                                           read_pages(output / 'ram-pages.tsv', first_frame=start + 1)):
                counts[0] += left is not None; counts[1] += right is not None
                if first is None and left != right:
                    first = {'frame': (left or right)[0], 'source_cycle': left[1] if left else None,
                             'native_cycle': right[1] if right else None,
                             'changed_pages': [f'{i*4096:06X}' for i in range(512) if left and right and left[2][i] != right[2][i]]}
            comparison = {'match': first is None and counts == [wanted - start, wanted - start], 'returns': counts,
                          'first_divergence': first}
        except (ValueError, OSError) as error:
            comparison = {'match': False, 'returns': counts, 'first_divergence': first, 'observation_error': str(error)}
    terminal_match = None; terminal_error = None
    if wanted == endpoint and result.returncode == 0:
        try:
            actual = terminal_consistency(output / 'ram-pages.tsv', output / f'ram-frame-{endpoint:06d}.bin', endpoint,
                                          first_frame=start + 1)
            terminal_match = actual == Path(reference['terminal_ram']).read_bytes()
        except (ValueError, OSError) as error:
            terminal_match = False; terminal_error = str(error)
    mechanical = bool(result.returncode == 0 and comparison and comparison['match'] and (wanted < endpoint or terminal_match))
    status = ('fail' if not mechanical else 'diagnostic' if not binary['binary_matches_setup'] or start
              else 'prefix_pass' if wanted < endpoint else 'pass')
    receipt = {'candidate_sha256': binary['binary_sha256'], **receipt_fields(binary),
               'source_reference': {'path': info['reference'], 'sha256': digest(Path(info['reference']))},
               'pad_ack_model': args.pad_ack_model, 'route_events': identity['console_events'],
               'diagnostic_prefix': wanted < endpoint, 'original_input_prefix_unchanged': True,
               'full_original_input_and_tail': wanted == endpoint, 'observed_returns': wanted,
               'native_input_exit': result.returncode, 'comparison': comparison, 'terminal_ram_match': terminal_match,
               'terminal_observation_error': terminal_error, 'mechanical_match': mechanical, 'status': status,
               'first_divergence': comparison['first_divergence'] if comparison else None,
               'streaming': streaming, 'checkpoints': checkpoint_receipt(args, wanted),
               'qualification': 'mechanical comparison only; ending/semantic review and repeated gameplay remain required'}
    write(output / 'source-comparison.json', receipt)
    print(json.dumps(receipt))
    return receipt


def run(args):
    check_launch_arguments(args)
    if args.ladder is not None:
        info = json.loads(args.setup.resolve(strict=True).read_text(encoding='utf-8'))
        endpoint = source.read(Path(info['reference'])).get('observed_returns')
        if type(endpoint) is not int:
            raise ValueError('invalid source reference endpoint')
        report = run_ladder(args.output.resolve(), parse_ladder(args.ladder, endpoint), endpoint,
                            lambda returns, target: replay(args, returns, target), write)
        return 0 if report['status'] in ('pass', 'prefix_pass') else 1
    receipt = replay(args, args.returns)
    return 0 if receipt['status'] in ('pass', 'prefix_pass') else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='action', required=True)
    build = sub.add_parser('setup')
    for name in ('project', 'reference', 'disc1', 'disc2', 'bios', 'movie'):
        build.add_argument('--' + name, type=Path, required=True)
    build.add_argument('--tools-dir', type=Path)
    build.add_argument('--jobs', type=int, default=4)
    build_cache.add_arguments(build)
    build.add_argument('--chdman', type=Path, help='chdman.exe for .chd input; otherwise PSX_CHDMAN or PATH')
    build.add_argument('--chd-cache', type=Path, help='private directory for split .chd tracks, keyed by container hash')
    play = sub.add_parser('run')
    play.add_argument('setup', type=Path)
    play.add_argument('output', type=Path)
    play.add_argument('--timeout', type=int, default=172800)
    play.add_argument('--pad-ack-model', choices=PAD_ACK_MODELS, default=PAD_ACK_MODELS[0],
                      help='digital pad ACK timing on both ports; measured on prefixes (no Nymashock digital model)')
    add_launch_arguments(play)
    args = parser.parse_args()
    return {'setup': setup, 'run': run}[args.action](args)


if __name__ == '__main__':
    raise SystemExit(main())
