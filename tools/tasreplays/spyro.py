"""Prepare and compare the Octoshock Spyro movies with owned inputs.

One adapter for the Spyro titles, keyed by title id. Spyro 3 4083M is one of the titles
spyro_admission.py admits; Spyro 2 5278M has an admission tool of its own, spyro2_admission.py
(hashed into its admitted reference, so it is not a second entry in the first module). ADMISSION
below names each title's tool, roles and longest neutral tail. Each entry pins its own boot
program, controller bytes, firmware and runtime profile; an entry is added when its title's route
is first built.

These movies carry a DualShock with live sticks. The route comes from
dualshock_route.read_movie_octoshock_dualshock, which keeps every logged stick byte, and every
profile names --axis-model octoshock-byte: Octoshock hands those bytes to the pad unscaled, so the
runtime must deliver them unchanged (the Nymashock default rescales them as 16-bit values). A
movie that presses the physical Analog (MODE) button is refused, here and in
run_native.route_identity, until a runtime models when the pad looks at it.

spyro2-5278M is a BizHawk 2.8 Nymashock movie. Its sticks are raw u16 values; the route comes
from nymashock28_route.read_movie_nymashock28, which converts them to the pad bytes the Nymashock
pad reports (Mednafen's table), so --axis-model octoshock-byte delivers them unscaled here too. The
movie declares the port-1 memory card off, and the reader refuses any other sync setting.

spyro3-4083M was recorded on BizHawk 2.3. Its profile is the 2.3 set Abe's Oddysee and Pepsiman
run (the shared 2.2.2 models with the 2.3 CD-DA and MDEC models), except that the pad answers
with the DualShock ACK timing: run_native refuses the digital Octoshock ACK model for a DualShock
route.

The source reference is the one spyro_admission.py admits. No reference is generated from a
candidate, and no movie input is changed to correct a divergence.
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
import dualshock_route
import media_container
import nymashock28_route
import spyro2_admission
import spyro_admission as source
import tekken3
from observation_evidence import terminal_consistency
from replay_prefix import native_span, write_prefix_route, parse_ladder, run_ladder
from launch_identity import resolve_binary, receipt_fields, add_launch_arguments, check_launch_arguments, run_native_arguments, checkpoint_resume, checkpoint_receipt
from stream_compare import Watcher, page_reference

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SCHEMA = 'spyro-tas-candidate-v1'
AXIS_MODEL = 'octoshock-byte'
PROFILE_23 = [
    '--critical-section-model', 'exception',
    '--field-model', 'octoshock-2.2.2-ntsc-raster',
    '--dma-model', 'octoshock-2.2.2-otc',
    '--pad-ack-model', 'nymashock-1.29.0-dualshock', '--legacy-card-repair', 'off',
    '--cd-firmware-model', 'octoshock-2.2.2', '--cd-cold-status-model', 'octoshock-2.2.2',
    '--cd-toc-seek-model', 'octoshock-2.2.2', '--cd-explicit-seek-model', 'octoshock-2.2.2',
    '--cd-read-start-model', 'octoshock-2.2.2-pipeline', '--cd-dma-model', 'octoshock-2.2.2',
    '--cd-cdda-model', 'octoshock-2.3', '--mdec-source-model', 'octoshock-2.3',
    '--gpu-status-model', 'octoshock-2.2.2-raster', '--gpu-dma-model', 'octoshock-2.2.2-bounded-quad',
    '--timer1-model', 'octoshock-2.2.2', '--timer2-model', 'octoshock-2.2.2',
    '--precise-slice', 'on', '--cpu-return-probe', '--ram-page-probe',
    '--axis-model', AXIS_MODEL,
]
# The Nymashock 2.8 host. Bio Hazard's set (biohazard.PROFILE: the nymashock-1.29.0 pad, CD drive
# and MDEC selectors; Mednafen 1.27.1 and 1.29.0 emulate the PS1 identically), without its
# --card-model because this movie declares port 1 empty, and with the octoshock-byte axis model for
# the pad bytes the route holds. One selector differs, found by divergence: this source takes CD
# Init's seek-jitter draw before its floor draw, so the drive model is octoshock-2.7, the same drive
# with that order (PS1G-125: with nymashock-1.29.0 the Init write at 400982724 drew 2736 where the
# source drew 1375; with octoshock-2.7 the replay matches the qualified source on 709 of 709 frames).
PROFILE_NYMA28 = [
    '--critical-section-model', 'exception', '--syscall-model', 'guest-exception',
    '--field-model', 'octoshock-2.2.2-ntsc-raster',
    '--dma-model', 'octoshock-2.2.2-otc',
    '--pad-ack-model', 'nymashock-1.29.0-dualshock', '--legacy-card-repair', 'off',
    '--cd-firmware-model', 'octoshock-2.2.2', '--cd-cold-status-model', 'octoshock-2.2.2',
    '--cd-toc-seek-model', 'octoshock-2.2.2', '--cd-explicit-seek-model', 'octoshock-2.2.2',
    '--cd-read-start-model', 'octoshock-2.2.2-pipeline', '--cd-dma-model', 'octoshock-2.2.2',
    '--cd-drive-model', 'octoshock-2.7',
    '--cd-cdda-model', 'octoshock-2.3', '--mdec-source-model', 'nymashock-1.29.0',
    '--gpu-status-model', 'octoshock-2.2.2-raster', '--gpu-dma-model', 'octoshock-2.2.2-bounded-quad',
    '--timer1-model', 'octoshock-2.2.2', '--timer2-model', 'octoshock-2.2.2',
    '--precise-slice', 'on', '--cpu-return-probe', '--ram-page-probe',
    '--axis-model', AXIS_MODEL,
]
SPYRO2_STEM = "Spyro 2 - Ripto's Rage! (USA)"
TITLES = {
    'spyro2-5278M': {
        # Canonical PSXRTI2 controller bytes of the movie (dualshock_route.write_route).
        'controller_sha256': 'bd1290ee904fb9e5c363ab8f19173ee265c49bc53b4d84d4010aebbdb94ed286',
        'route_reader': 'nymashock28', 'reader': {'mednafen_values': {'psx.input.port1.memcard': '0'}},
        'bios_stem': 'SCPH5501',
        'boot': 'SCUS_944.25', 'game_id': 'SCUS-94425',
        'exe_sha256': '7b54002789ab379e0d3a6b49d7b50078b9c9c2fed589f41ff9878ebb68fe0bc1',
        'disc_stem': SPYRO2_STEM, 'track_bytes': 565371408,
        # Read from the boot program's own PS-X EXE header and cross-checked on every setup.
        'load_address': 0x80010000, 'entry_pc': 0x8005478C, 'text_size': 0x57000, 'stack_base': 0x801FFFF0,
        'system_cnf': b'BOOT = cdrom:\\SCUS_944.25;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFFF0\r\n',
        'name': 'Spyro 2 Riptos Rage', 'exe_name': 'Spyro2-TAS',
        'profile': PROFILE_NYMA28,
    },
    'spyro3-4083M': {
        # Canonical PSXRTI2 controller bytes of the movie (dualshock_route.write_route).
        'controller_sha256': '3160bdc3b0d55034196b6b0dae18cb1433d3fae2523cab6f8ccb6b68319f0cd1',
        'reader': {'emu_versions': ('Version 2.3.0',)},
        'bios_stem': 'SCPH5501',
        'boot': 'SCUS_944.67', 'game_id': 'SCUS-94467',
        'exe_sha256': 'cb819ee78c556d403779309859cb08a7111331f624759bc1bc380946261bb26e',
        'disc_stem': 'Spyro - Year of the Dragon (USA)', 'track_bytes': 597250416,
        # Read from the boot program's own PS-X EXE header and cross-checked on every setup.
        'load_address': 0x80010000, 'entry_pc': 0x80059444, 'text_size': 0x5C800, 'stack_base': 0x801FFFF0,
        'system_cnf': b'BOOT = cdrom:\\SCUS_944.67;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFFF0\r\n',
        'name': 'Spyro Year of the Dragon', 'exe_name': 'Spyro3-TAS',
        'profile': PROFILE_23,
    },
}

digest = tekken3.digest
write = tekken3.write_json
command = tekken3.command
require_hash = tekken3.require_hash


# The admission facts of a title that spyro_admission.py does not admit. spyro2_admission.py is a
# module of its own because its bytes are hashed into the admitted reference.
SPYRO2_ADMITTED = {
    'frames': spyro2_admission.FRAMES, 'schema': 'spyro2-independent-source-v1',
    'movie_sha256': spyro2_admission.MOVIE_SHA, 'firmware': spyro2_admission.FIRMWARE_NAME,
    'firmware_sha256': spyro2_admission.FIXED[spyro2_admission.FIRMWARE_NAME],
    'media': {name: sha for name, sha in spyro2_admission.FIXED.items() if name.startswith(SPYRO2_STEM + '.')},
    'tool': spyro2_admission, 'roles': ('stock-control', 'pages-observer'), 'max_tail': 12000,
}


def title_spec(title):
    """(runtime entry, admission facts); facts carry the admission tool, its roles and longest tail."""
    if title not in TITLES:
        raise ValueError('no Spyro route entry for ' + str(title))
    if title == 'spyro2-5278M':
        return TITLES[title], SPYRO2_ADMITTED
    spec = source.TITLES[title]
    return TITLES[title], {**spec, 'firmware_sha256': source.FIRMWARE[spec['firmware']][1],
                           'tool': source, 'roles': ('stock', 'observer'), 'max_tail': 6000}


def admit(title, root, role):
    """The title's own admission tool, called with the arguments it takes."""
    _, spec = title_spec(title)
    return spec['tool'].admit(root, role) if spec['tool'] is not source else source.admit(root, title, role)


def verify_reference(path, title):
    """The admitted source reference, re-checked against this title's own constants."""
    _, spec = title_spec(title)
    frames, tool = spec['frames'], spec['tool']
    ref = tool.read(path)
    if (ref.get('schema') != spec['schema'] or ref.get('source_qualification') != 'pass'
            or ref.get('movie_sha256') != spec['movie_sha256'] or ref.get('original_inputs') != frames):
        raise ValueError('wrong independent Spyro source reference')
    endpoint = ref.get('observed_returns')
    if type(endpoint) is not int or not frames <= endpoint <= frames + spec['max_tail'] or ref.get('neutral_tail') != endpoint - frames:
        raise ValueError('invalid source reference endpoint')
    bindings = {str(Path(x['path']).resolve()): x['sha256'] for x in ref['bindings']}
    if len(bindings) != len(ref['bindings']):
        raise ValueError('duplicate source reference binding')
    for p, h in bindings.items():
        require_hash(Path(p), h)
    for key in ('ram_pages', 'terminal_ram'):
        if str(Path(ref[key]).resolve()) not in bindings:
            raise ValueError('unbound source reference field')
    if ref.get('admission_tool_sha256') != tool.digest(Path(tool.__file__)):
        raise ValueError('source reference admission tool differs')
    stock, observed = Path(ref['stock_source']).resolve(), Path(ref['observer_source']).resolve()
    if stock == observed:
        raise ValueError('source reference roles must be independent')
    if (Path(ref['ram_pages']).resolve() != observed / 'ram-pages.tsv'
            or Path(ref['terminal_ram']).resolve() != observed / f'ram-frame-{endpoint:06d}.bin'):
        raise ValueError('source reference fields do not belong to the admitted roles')
    for root, role in zip((stock, observed), spec['roles']):
        for name in ('manifest.json', 'complete.json', 'exit.json', 'semantic-review.json'):
            if str(root / name) not in bindings:
                raise ValueError('source reference is missing admission evidence')
        admitted, terminal, _ = admit(title, root, role)
        if admitted != endpoint or terminal[3].lower() != ref['terminal_ram_sha256']:
            raise ValueError('source reference terminal differs')
    return ref


def read_route(movie, title):
    """The movie's rows, refused when a runtime could not deliver them exactly."""
    entry, spec = title_spec(title)
    require_hash(movie, spec['movie_sha256'])
    if entry.get('route_reader') == 'nymashock28':
        rows, facts = nymashock28_route.read_movie_nymashock28(movie, **entry['reader'])
    else:
        rows, facts = dualshock_route.read_movie_octoshock_dualshock(movie, **entry['reader'])
    if facts['mode_press_frames']:
        raise ValueError('movie presses the physical Analog button; no runtime models it yet')
    if len(rows) != spec['frames']:
        raise ValueError('incomplete original movie')
    return rows


def media(cue, bios, title):
    entry, spec = title_spec(title)
    stem = entry['disc_stem']
    require_hash(cue, spec['media'][stem + '.cue'])
    require_hash(bios, spec['firmware_sha256'])
    lines = [x.strip() for x in cue.read_text().splitlines() if x.strip()]
    match = re.fullmatch(r'FILE "([^"\r\n]+)" BINARY', lines[0]) if lines else None
    if not match or lines[1:] != ['TRACK 01 MODE2/2352', 'INDEX 01 00:00:00']:
        raise ValueError('wrong single-track %s topology' % stem)
    track = (cue.parent / match[1]).resolve(strict=True)
    if not track.is_relative_to(cue.parent.resolve()) or track.stat().st_size != entry['track_bytes']:
        raise ValueError('invalid %s data track' % stem)
    require_hash(track, spec['media'][stem + '.bin'])
    return track


def boot_program(track, title):
    entry, _ = title_spec(title)
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
        if span(*tree['SYSTEM.CNF']) != entry['system_cnf']:
            raise ValueError('wrong boot configuration')
        data = span(*tree[entry['boot']])
    if hashlib.sha256(data).hexdigest() != entry['exe_sha256']:
        raise ValueError('wrong %s executable' % entry['name'])
    if data[:8] != b'PS-X EXE':
        raise ValueError('boot program is not a PS-X EXE')
    pc0, _, t_addr, t_size = struct.unpack_from('<IIII', data, 0x10)
    stack = struct.unpack_from('<I', data, 0x30)[0]
    if (pc0, t_addr, t_size, stack) != (entry['entry_pc'], entry['load_address'], entry['text_size'], entry['stack_base']):
        raise ValueError('boot program header differs from the pinned game profile: '
                         'pc0=%08X load=%08X text=%X stack=%08X' % (pc0, t_addr, t_size, stack))
    return data


def setup(args):
    title = args.title
    entry, spec = title_spec(title)
    bios_stem, boot = entry['bios_stem'], entry['boot']
    bios_sha = spec['firmware_sha256']
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
    verify_reference(reference_path, title)
    container = media_container.resolve_disc(args.disc, cache=args.chd_cache, chdman=args.chdman, stem=entry['disc_stem'])
    disc, bios, movie = [p.resolve(strict=True) for p in (container, args.bios, args.movie)]
    track = media(disc, bios, title)
    rows = read_route(movie, title)
    project = args.project.resolve()
    if project == ROOT or project.is_relative_to(ROOT):
        raise ValueError('put generated title/build output outside source')
    project.mkdir(parents=True, exist_ok=False)
    data = boot_program(track, title)
    exe = project / boot
    exe.write_bytes(data)
    staged_bios = project / (bios_stem + '.BIN')
    shutil.copyfile(bios, staged_bios)
    require_hash(staged_bios, bios_sha)
    tape = project / 'cold-random.psxrng'
    command([sys.executable, HERE / 'external/source_random_tape.py', tape], project / 'random-tape.log')
    require_hash(tape, tekken3.TAPE_SHA)
    route = project / 'input.psxrti2'
    receipt = dualshock_route.write_route(rows, route)
    if receipt['canonical_controller_sha256'] != entry['controller_sha256']:
        raise ValueError('original controller bytes differ')
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
    profile = (ROOT / ('bios/' + bios_stem + '.toml')).read_text()
    for key, value in [('rom', staged_bios), ('seeds', ROOT / 'recompiler/seeds/phase2_ghidra_seeds.json'), ('out_dir', ROOT / 'generated')]:
        profile = re.sub(r'^' + key + r'\s*=.*$', lambda _, v=value: key + ' = ' + q(v), profile, flags=re.M)
    bios_profile.write_text(profile, encoding='utf8')
    game = project / 'game.toml'
    game.write_text(f'''[game]
name = "{entry['name']} TAS"
id = "{entry['game_id']}"
exe = {q(exe)}
load_address = "0x{entry['load_address']:08X}"
entry_pc = "0x{entry['entry_pc']:08X}"
text_size = "0x{entry['text_size']:X}"
stack_base = "0x{entry['stack_base']:08X}"
[recompiler]
seeds = {q(project / 'seeds.txt')}
bios_config = {q(bios_profile)}
strict = true
out_dir = {q(project / 'generated')}
[runtime]
window_title = "{entry['name']} - original TAS"
bios_hle = false
[video]
renderer = "software"
''', encoding='utf8')

    def stamp():
        fingerprint = subprocess.check_output([str(bash), (ROOT / 'tools/bios_emitter_fingerprint.sh').as_posix(),
                                               bios_profile.as_posix()], cwd=ROOT, text=True).strip()
        if not re.fullmatch('[0-9a-f]{64}', fingerprint):
            raise ValueError('invalid generated BIOS fingerprint')
        (ROOT / ('generated/' + bios_stem + '.emitter.sha')).write_text(fingerprint + '\n')
        return fingerprint
    stamped = []

    def generate():
        command([tools / 'psxrecomp-toml.exe', exe, '--output', project / 'census.toml', '--seeds', project / 'seeds.txt'], project / 'census.log')
        command([tools / 'psxrecomp-bios.exe', '--config', bios_profile, '--rom', staged_bios,
                 '--out-dir', ROOT / 'generated'], project / 'generate-bios.log')
        stamped.append(stamp())
        command([tools / 'psxrecomp-game.exe', '--config', game], project / 'generate-game.log')
    generated_record = build_cache.stage_generated(cache_root, ROOT, project, bios_stem,
        lambda: build_cache.generated_inputs(tools, bios_stem, staged_bios, ROOT / 'recompiler/seeds/phase2_ghidra_seeds.json',
                                             build_cache.blob_id(ROOT, 'bios/' + bios_stem + '.toml'),
                                             build_cache.portable_emitter_fingerprint(bash, ROOT, bios_profile), exe, game),
        True, generate, build_head)
    # On a cache hit the emitter fingerprint is recomputed and written exactly as on a miss.
    fingerprint = stamped[0] if stamped else stamp()
    native = project / 'native'
    native_argv = ['cmake', '-S', HERE, '-B', native, *common, '-DTAS_PROJECT_DIR=' + str(project),
                   '-DTAS_GAME_STEM=' + boot, '-DTAS_EXE_NAME=' + entry['exe_name'],
                   '-DTAS_WINDOW_TITLE=' + entry['name'] + ' TAS',
                   '-DPSXRECOMP_BIOS_STEMS=' + bios_stem, '-DPSX_SHELLWIN_INTERP=ON',
                   '-DPSXRECOMP_BIOS_PROFILE=' + str(bios_profile), '-D_psxrt_bash=' + str(bash),
                   '-DPSX_RECOMP_UI=OFF', '-DPSX_NETPLAY=OFF', '-DPSX_REWIND=OFF', '-DPSX_SETUP_WIZARD=OFF',
                   '-DPSX_DEBUG_TOOLS=ON', '-DPSX_ENABLE_VULKAN=OFF',
                   '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE', '-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE']

    def build_native():
        command(native_argv, project / 'configure-native.log', env=build_cache.build_env())
        command(['cmake', '--build', native, '--parallel', str(args.jobs)], project / 'build-native.log', env=build_cache.build_env())
    native_record = build_cache.stage_native(cache_root, ROOT, project, native, entry['exe_name'],
        lambda: build_cache.native_inputs(ROOT, build_cache.generated_set(ROOT, bios_stem, project), native_argv, bios_profile),
        build_native, build_head)
    if (subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip()
            or subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip() != build_head):
        raise ValueError('source changed during candidate build')
    files = [disc, track, bios, staged_bios, bios_profile, movie, exe, game, tape, route,
             project / 'seeds.txt', reference_path]
    build = native / (entry['exe_name'] + '.exe')
    generated = {str(f.relative_to(ROOT)): digest(f) for f in (ROOT / 'generated').glob(bios_stem + '*') if f.is_file()}
    generated.update({str(f): digest(f) for f in (project / 'generated').glob('*') if f.is_file()})
    info = {'schema': SCHEMA, 'title': title, 'source_head': build_head,
            'source_tree': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD^{tree}'], text=True).strip(),
            'reference': str(reference_path), 'bindings': [{'path': str(f), 'sha256': digest(f)} for f in files],
            'generated': generated, 'executable': str(build), 'executable_sha256': digest(build),
            'disc': str(disc), 'bios': str(staged_bios), 'game': str(game), 'route': str(route), 'tape': str(tape),
            'profile': entry['profile'], 'bios_emitter_fingerprint': fingerprint,
            'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
            'tools_build_dir': str(tools),
            'build_cache': build_cache.receipt_block(cache_root, tools_record, generated_record, native_record),
            'qualification': 'candidate only; source/native comparison pending'}
    write(project / 'setup.json', info)
    print(json.dumps({'candidate': str(build), 'sha256': digest(build)}))


def read_setup(path):
    info = json.loads(Path(path).resolve(strict=True).read_text())
    if info.get('schema') != SCHEMA:
        raise ValueError('wrong Spyro candidate')
    title_spec(info.get('title'))
    return info


def replay(args, returns=None, output=None):
    """One replay: full when returns is None or the endpoint, otherwise a diagnostic prefix."""
    output = (args.output if output is None else Path(output)).resolve()
    info = read_setup(args.setup)
    title = info['title']
    entry, spec = title_spec(title)
    for binding in info['bindings']:
        require_hash(Path(binding['path']), binding['sha256'])
    if args.exe is None and args.diagnostic_binary is None:
        require_hash(Path(info['executable']), info['executable_sha256'])
    binary = resolve_binary(info['executable'], info['executable_sha256'], args.exe, args.diagnostic_binary)
    if info['profile'] != entry['profile']:
        raise ValueError('candidate profile differs; build a new candidate')
    reference = verify_reference(Path(info['reference']), title)
    endpoint = reference['observed_returns']
    if output.exists():
        raise ValueError('fresh output required')
    output.parent.mkdir(parents=True, exist_ok=True)
    wanted = endpoint if returns is None else returns
    # A resumed run compares only the returns after its checkpoint and never qualifies.
    start = checkpoint_resume(args)[0]
    if start >= wanted:
        raise ValueError('the checkpoint is not before the compared endpoint')
    records, tail = native_span(spec['frames'], endpoint, wanted)
    route = Path(info['route'])
    if wanted < endpoint:
        route = write_prefix_route(route, records, output.with_name(output.name + '-input.psxrti2'))
    argv = [sys.executable, str(HERE / 'run_native.py'), str(output), '--exe', binary['path'], '--game', info['game'],
            '--route', str(route), '--disc', info['disc'], '--bios', info['bios'],
            '--cd-source-clock-tape', info['tape'], '--neutral-tail', str(tail), '--timeout', str(args.timeout),
            '--checkpoint-every', '1200', '--renderer', 'software', '--storage-budget-mib', '3072',
            '--ram-snapshot-frame', str(wanted), *run_native_arguments(args, binary, wanted), *entry['profile']]
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
    receipt = {'title': title, 'candidate_sha256': binary['binary_sha256'], **receipt_fields(binary),
               'source_reference': {'path': info['reference'], 'sha256': digest(Path(info['reference']))},
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
        info = read_setup(args.setup)
        endpoint = title_spec(info['title'])[1]['tool'].read(Path(info['reference'])).get('observed_returns')
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
    build.add_argument('--title', choices=sorted(TITLES), required=True)
    for name in ('project', 'reference', 'disc', 'bios', 'movie'):
        build.add_argument('--' + name, type=Path, required=True)
    build.add_argument('--tools-dir', type=Path)
    build.add_argument('--jobs', type=int, default=4)
    build_cache.add_arguments(build)
    build.add_argument('--chdman', type=Path, help='chdman.exe for .chd input; otherwise PSX_CHDMAN or PATH')
    build.add_argument('--chd-cache', type=Path, help='private directory for split .chd tracks, keyed by container hash')
    play = sub.add_parser('run')
    play.add_argument('setup', type=Path)
    play.add_argument('output', type=Path)
    play.add_argument('--timeout', type=int, default=129600)
    add_launch_arguments(play)
    args = parser.parse_args()
    return {'setup': setup, 'run': run}[args.action](args)


if __name__ == '__main__':
    raise SystemExit(main())
