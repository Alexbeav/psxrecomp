"""Serial real-title host-pacing comparison; never shows a window or touches stage.

Only the frame_pacing object is recompiled. Every other linked input is reused.
This is an enhanced player workload, never a strict console replay benchmark.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import time

CORE = Path(__file__).resolve().parents[1]
LANE = CORE.parent
PAIR = LANE.parent / 'PS1B-165-pair'
BUILD = PAIR / 'b'
OUT = LANE / 'reports' / 'consumer-789'
SDK = LANE / 'inputs/toolchain/bin'
FAMILY = '789'
FRAMES = 600
PLAYBACK = None
FLAGS = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def command(args, cwd=BUILD):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                            encoding='utf-8', errors='replace', creationflags=FLAGS)
    if result.returncode:
        raise RuntimeError(f'{args!r}\n{result.stdout}\n{result.stderr}')
    return result.stdout


def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    if (OUT / 'prepared.json').exists():
        raise RuntimeError('Preparation evidence exists; do not overwrite')
    commands = command([SDK / 'ninja.exe', '-t', 'commands', 'psx-runtime']).splitlines()
    source_name, macro = {'789': ('frame_pacing.c', 'PSX_NATIVE_HOST_PACING'),
                          '784': ('gpu_gl_renderer.c', 'PSX_SHARED_PRESENT_IMAGES'),
                          '788': ('boot_state_capture_jobs.c', 'PSX_NATIVE_CAPTURE_JOBS')}[FAMILY]
    compile_line = next(row for row in commands if row.endswith('/' + source_name))
    obj = re.search(r' -o (\S+) -c ', compile_line).group(1)
    link = json.loads(command([SDK / 'ninja.exe', '-t', 'compdb', '-x',
                               'CXX_EXECUTABLE_LINKER__psx-runtime_Release']))[0]['command']
    linker = str(SDK / 'clang++.exe')
    start = link.index(linker) + len(linker)
    link_args = link[start:].split(' && ')[0].strip()
    # Feed the expanded object list through a response file, avoiding Windows' command limit.
    entries = []
    for policy, selected in [('reference', 0), ('native', 1)]:
        output_obj = (OUT / (policy + '.obj')).as_posix()
        compile_cmd = compile_line.replace(f'-D{macro}=1', f'-D{macro}={selected}')
        assert compile_cmd != compile_line or selected == 1
        compile_cmd = compile_cmd.replace(obj, output_obj).replace(
            obj.replace('/', '\\'), output_obj)
        extra_replacement = None
        if FAMILY == '788':
            source = (CORE / 'runtime/src/boot_state_capture_jobs.c').read_text(encoding='utf-8')
            source = source.replace('#define PSX_NATIVE_CAPTURE_JOBS 1', f'#define PSX_NATIVE_CAPTURE_JOBS {selected}')
            source = r'''
#include <stdlib.h>
#include <stdio.h>
static unsigned long long consumer_allocs, consumer_reallocs;
static void *consumer_malloc(size_t n) { ++consumer_allocs; return malloc(n); }
static void *consumer_realloc(void *p,size_t n) { ++consumer_reallocs; return realloc(p,n); }
void consumer_snapshot_stats(void) { fprintf(stderr,"[CAPTURE_ALLOC] malloc=%llu realloc=%llu\n",consumer_allocs,consumer_reallocs); }
#define malloc consumer_malloc
#define realloc consumer_realloc
''' + source
            instrumented = OUT / (policy + '-snapshot.c')
            instrumented.write_text(source, encoding='utf-8')
            original = (CORE / 'runtime/src/boot_state_capture_jobs.c').as_posix()
            compile_cmd = compile_cmd.replace(original, instrumented.as_posix())
            compile_cmd += ' -I' + (CORE / 'runtime/src').as_posix()
            rewind_cmd = next(row for row in commands if row.endswith('/psx_rewind.c'))
            rewind_obj = re.search(r' -o (\S+) -c ', rewind_cmd).group(1)
            new_rewind = (OUT / (policy + '-rewind.obj')).as_posix()
            rewind_cmd = rewind_cmd.replace(f'-D{macro}=1', f'-D{macro}={selected}')
            rewind_cmd = rewind_cmd.replace(rewind_obj, new_rewind).replace(rewind_obj.replace('/', '\\'), new_rewind)
            rewind_source = (CORE / 'runtime/src/psx_rewind.c').read_text(encoding='utf-8')
            marker = '    capture_thumb(thumb);\n    list_push(tick, thumb);'
            assert rewind_source.count(marker) == 1
            witness = r'''
    {
        size_t length=0;
        const uint8_t *raw=rbe_snap_ring_peek(s_ring,tick,&length);
        uint64_t hash=UINT64_C(14695981039346656037);
        if (!raw || !length) abort();
        for(size_t i=0;i<length;i++) hash=(hash^raw[i])*UINT64_C(1099511628211);
        fprintf(stderr,"[SNAPSHOT_WITNESS] tick=%u bytes=%llu hash=%016llx\n",tick,(unsigned long long)length,(unsigned long long)hash);
        extern void consumer_snapshot_stats(void);
        consumer_snapshot_stats();
    }
'''
            rewind_source = rewind_source.replace(marker, witness + marker)
            instrumented_rewind = OUT / (policy + '-rewind.c')
            instrumented_rewind.write_text(rewind_source, encoding='utf-8')
            rewind_cmd = rewind_cmd.replace((CORE / 'runtime/src/psx_rewind.c').as_posix(), instrumented_rewind.as_posix())
            rewind_cmd += ' -I' + (CORE / 'runtime/src').as_posix()
            command(rewind_cmd)
            extra_replacement = rewind_obj, new_rewind
        if FAMILY == '784':
            source = (CORE / 'runtime/src/gpu_gl_renderer.c').read_text(encoding='utf-8')
            marker = 'static void hold_publish_interpolated('
            witness = r'''
/* Evaluation-only readback from the actual image published to hold-last. */
static void consumer_present_witness(void) {
    static unsigned sequence;
    ++sequence;
    if (sequence != 1 && sequence != 60 && sequence != 120 && sequence != 240 && sequence != 480) return;
    int w = s_hold_image.w, h = s_hold_image.h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !s_hold_image.tex) abort();
    size_t bytes = (size_t)w * h * 4;
    unsigned char *pixels = malloc(bytes);
    if (!pixels) abort();
    GLint old;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    glBindTexture(GL_TEXTURE_2D, s_hold_image.tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i=0;i<bytes;i++) hash=(hash^pixels[i])*UINT64_C(1099511628211);
    free(pixels);
    fprintf(stderr,"[PRESENT_WITNESS] capture=%u width=%d height=%d hash=%016llx\n",sequence,w,h,(unsigned long long)hash);
}
'''
            assert source.count(marker) == 1
            source = source.replace(marker, witness + marker)
            first = source.index(marker)
            last = source.index('static void interp_draw_textures', first)
            block = source[first:last]
            assert block.count('        return;') == 1
            block = block.replace('        return;', '        consumer_present_witness();\n        return;')
            tail = '    hold_capture_native_fbo(source_fbo, x, y, w, h, force_4_3, linear);'
            assert tail in block
            block = block.replace(tail, tail + '\n    consumer_present_witness();')
            source = source[:first] + block + source[last:]
            instrumented = OUT / (policy + '-renderer.c')
            instrumented.write_text(source, encoding='utf-8')
            original = (CORE / 'runtime/src/gpu_gl_renderer.c').as_posix()
            assert original in compile_cmd
            compile_cmd = compile_cmd.replace(original, instrumented.as_posix())
            compile_cmd += ' -I' + (CORE / 'runtime/src').as_posix()
        (OUT / (policy + '-compile-command.txt')).write_text(compile_cmd, encoding='utf-8')
        command(compile_cmd)
        exe = BUILD / f'Ape_Consumer_{FAMILY}_{policy}.exe'
        args = link_args.replace(obj, output_obj).replace(obj.replace('/', '\\'), output_obj)
        if extra_replacement:
            old, new = extra_replacement
            args = args.replace(old, new).replace(old.replace('/', '\\'), new)
        assert output_obj in args
        args = args.replace(' -o Ape_Modern_Pair.exe ', f' -o {exe.as_posix()} ')
        args = args.replace('libApe_Modern_Pair.dll.a', (OUT / (policy + '.dll.a')).as_posix())
        assert '-o Ape_Modern_Pair.exe' not in args
        rsp = OUT / (policy + '-link.rsp')
        rsp.write_text(args, encoding='utf-8')
        command([linker, '@' + str(rsp)])
        shutil.copy2(BUILD / 'Ape_Modern_Pair.execution.json', exe.with_suffix('.execution.json'))
        entries.append({'policy': policy, 'selected': selected, 'exe': str(exe),
                        'exe_sha256': sha(exe), 'object_sha256': sha(output_obj)})
    record = {'variants': entries, 'baseline_exe_sha256': sha(BUILD / 'Ape_Modern_Pair.exe'),
              'ninja_log_sha256': sha(BUILD / '.ninja_log'),
              'family': FAMILY, 'modified_production_sources': False, 'generated_title_compiler_calls': 0}
    (OUT / 'prepared.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps(record))


def pgo_ready(phase):
    config = json.loads((LANE / 'reports/consumer-790-config.json').read_text(encoding='utf-8-sig'))
    pgo_build = Path(config['build'])
    phase_record = json.loads((pgo_build / f'runtime-hot-pgo/psx-runtime/{phase}.json').read_text(encoding='utf-8'))
    if phase_record['status'] != 'built':
        raise RuntimeError('PGO phase has no successful build')
    source = pgo_build / f'runtime-hot-pgo/psx-runtime/artifacts/{phase}/Ape_Modern_Pair.exe'
    policy = {'baseline': 'reference', 'generate': 'train', 'use': 'native'}[phase]
    exe = pgo_build / f'Ape_PGO_{policy}.exe'
    if exe.exists():
        raise RuntimeError('Phase consumer exists; preserve original evidence')
    shutil.copy2(source, exe)
    shutil.copy2(source.with_suffix('.execution.json'), exe.with_suffix('.execution.json'))
    cc = next(a.split('=', 1)[1] for a in config['configure_args'] if a.startswith('-DCMAKE_C_COMPILER='))
    for name in ('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll'):
        dependency = Path(cc).parent / name
        if not dependency.is_file():
            raise RuntimeError('Missing GCC runtime: ' + str(dependency))
        shutil.copy2(dependency, pgo_build / name)
    OUT.mkdir(exist_ok=True)
    path = OUT / 'prepared.json'
    prepared = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {
        'variants': [], 'baseline_exe_sha256': sha(BUILD / 'Ape_Modern_Pair.exe'),
        'ninja_log_sha256': sha(BUILD / '.ninja_log')}
    prepared['variants'].append({'policy': policy, 'exe': str(exe), 'exe_sha256': sha(exe), 'phase': phase})
    path.write_text(json.dumps(prepared, indent=2), encoding='utf-8')
    print(json.dumps(prepared['variants'][-1]))


def anchor_ready():
    OUT.mkdir(exist_ok=True)
    path = OUT / 'prepared.json'
    if path.exists(): raise RuntimeError('Anchor preparation evidence exists')
    exe = BUILD / 'Ape_Modern_Pair.exe'
    record = {'variants': [{'policy': 'native', 'exe': str(exe), 'exe_sha256': sha(exe)}],
              'baseline_exe_sha256': sha(exe), 'ninja_log_sha256': sha(BUILD / '.ninja_log')}
    path.write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps(record))


def visible_windows(pid):
    result = []
    user = ctypes.windll.user32
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            result.append(int(hwnd))
        return True
    callback = callback_type(visit)
    user.EnumWindows(callback, 0)
    return result


def run(policy, sample):
    prepared = json.loads((OUT / 'prepared.json').read_text(encoding='utf-8'))
    variant = next(row for row in prepared['variants'] if row['policy'] == policy)
    qa = OUT / (f'{"play" if PLAYBACK else "record"}-{sample}' if FAMILY == 'anchor' else f'{policy}-{sample}')
    qa.mkdir()
    env = os.environ.copy()
    env['PATH'] = str(SDK) + os.pathsep + str(LANE / 'inputs/SDL3/bin') + os.pathsep + env.get('PATH', '')
    for key in list(env):
        if key.startswith(('PSX_REPLAY_', 'PSX_RUNTIME_PERF_')):
            env.pop(key)
    env.update(SDL_AUDIODRIVER='dummy', PSX_VSYNC='0', PSX_REPLAY_RECORD_BOOT='1',
               PSX_REPLAY_RECORD_FRAMES=str(FRAMES), PSX_REPLAY_TEST_INPUT_SEED='165',
               PSX_REPLAY_EXIT_AT_END='1', PSX_RUNTIME_PERF_DIAG='1',
               PSX_RUNTIME_PERF_DIAG_MS='1000', PSX_BENCH_WINDOW='120:600',
               TEMP=str(PAIR / 'tmp'), TMP=str(PAIR / 'tmp'), PYTHONDONTWRITEBYTECODE='1')
    if FAMILY == '784':
        env.update(PSX_FRAME_INTERPOLATION='1', PSX_FRAME_INTERPOLATION_FPS='120',
                   PSX_GL_INTERP_DIAG='1', PSX_RENDER_THREAD='1', PSX_INTERNAL_RESOLUTION='native')
    if FAMILY == '788':
        env.update(PSX_REWIND='1', PSX_REWIND_INTERVAL='4', PSX_REWIND_DEPTH='50')
    config = PAIR / 'p/game.toml'
    if FAMILY == '784' and FRAMES > 600:
        body = config.read_text(encoding='utf-8').replace('aspect_ratio = "4:3"', 'aspect_ratio = "16:9"')
        body = body.replace('"disc/SCUS_944.23"', '"' + (PAIR / 'p/disc/SCUS_944.23').as_posix() + '"')
        body = body.replace('"disc/Ape Escape (USA).cue"', '"' + (PAIR / 'p/disc/Ape Escape (USA).cue').as_posix() + '"')
        config = qa / 'game.toml'
        config.write_text(body, encoding='utf-8')
    args = [variant['exe'], '--hidden-window', '--no-launcher', '--renderer',
            'opengl' if FAMILY == '784' else 'software',
            '--game', str(config), '--disc', str(PAIR / 'p/disc/Ape Escape (USA).cue'),
            '--bios', str(CORE / 'bios/SCPH1001.BIN'), '--memcard-dir', str(qa / 'cards')]
    if FAMILY == 'anchor':
        args[1] = '--headless'
        env.pop('PSX_REPLAY_RECORD_BOOT', None)
        if PLAYBACK:
            env.pop('PSX_REPLAY_RECORD_FRAMES', None)
            env.pop('PSX_REPLAY_TEST_INPUT_SEED', None)
            args += ['--replay', str(PLAYBACK), '--replay-verdict', str(qa / 'verdict.json'), '--replay-fast']
        else:
            env.update(PSX_REPLAY_RECORD_FILE=str(qa / 'record.psxrpl'), PSX_REPLAY_RECORD_AT='120')
    foreground_before = ctypes.windll.user32.GetForegroundWindow()
    started = time.monotonic()
    visible = []
    with (qa / 'stdout.log').open('w', encoding='utf-8') as log, (qa / 'stderr.log').open('w', encoding='utf-8') as errors:
        child = subprocess.Popen(args, cwd=BUILD, env=env, stdout=log,
                                 stderr=errors, creationflags=FLAGS)
        kernel = ctypes.windll.kernel32
        times = [wintypes.FILETIME() for _ in range(4)]
        try:
            while child.poll() is None:
                visible.extend(visible_windows(child.pid))
                if visible:
                    raise RuntimeError('Unexpected visible owned window')
                if time.monotonic() - started > 120:
                    raise TimeoutError('Bounded title run exceeded 120 seconds')
                time.sleep(.05)
            kernel.GetProcessTimes(wintypes.HANDLE(int(child._handle)), *[ctypes.byref(t) for t in times])
        finally:
            if child.poll() is None:
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'],
                               capture_output=True, text=True, encoding='utf-8', errors='replace',
                               creationflags=FLAGS)
                child.wait()
    elapsed = time.monotonic() - started
    def ticks(t): return (t.dwHighDateTime << 32) | t.dwLowDateTime
    text = (qa / 'stdout.log').read_text(encoding='utf-8') + '\n' + (qa / 'stderr.log').read_text(encoding='utf-8')
    bench = [line for line in text.splitlines() if line.startswith('[BENCH]')]
    replays = list(qa.rglob('*.psxrpl'))
    record = {'policy': policy, 'sample': sample, 'frames': FRAMES, 'exit_code': child.returncode,
              'rewind_interval': 4 if FAMILY == '788' else None,
              'seconds': elapsed, 'cpu_ms': (ticks(times[2]) + ticks(times[3])) / 10000,
              'bench': bench, 'visible_windows': visible,
              'presentation_witnesses': [line for line in text.splitlines() if '[PRESENT_WITNESS]' in line],
              'snapshot_witnesses': [line for line in text.splitlines() if '[SNAPSHOT_WITNESS]' in line],
              'snapshot_allocations': [line for line in text.splitlines() if '[CAPTURE_ALLOC]' in line],
              'foreground_unchanged': ctypes.windll.user32.GetForegroundWindow() == foreground_before,
              'replays': [str(p) for p in replays], 'command': args,
              'baseline_exe_unchanged': sha(BUILD / 'Ape_Modern_Pair.exe') == prepared['baseline_exe_sha256'],
              'ninja_log_unchanged': sha(BUILD / '.ninja_log') == prepared['ninja_log_sha256']}
    if PLAYBACK and (qa / 'verdict.json').exists():
        record['verdict'] = json.loads((qa / 'verdict.json').read_text(encoding='utf-8'))
    (qa / 'result.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in record.items() if k not in ('command', 'snapshot_witnesses', 'snapshot_allocations')}))
    if PLAYBACK:
        if child.returncode or record.get('verdict', {}).get('result') != 'in_sync':
            raise RuntimeError('Anchored playback failed; inspect saved verdict and logs')
    elif child.returncode or not bench or not replays or visible or (FAMILY == '784' and not record['presentation_witnesses']) or (FAMILY == '788' and not record['snapshot_witnesses']):
        raise RuntimeError('Consumer evidence incomplete; inspect stdout.log')


def summarize():
    rows = []
    guest = []
    for path in sorted(OUT.glob('*-*/result.json')):
        row = json.loads(path.read_text(encoding='utf-8'))
        if row.get('frames', 600) != FRAMES:
            continue
        if FAMILY == '788' and not row.get('bench'):
            continue  # Earlier merged-stream evidence is retained but incomplete.
        if FAMILY == '788' and row.get('rewind_interval') != 4:
            continue
        data = Path(row['replays'][0]).read_bytes()
        assert data[:8] == b'PSXRTI3\0'
        version, size, frames, flags, extension = struct.unpack_from('<5I', data, 8)
        assert version == 3 and flags == 0 and len(data) == 28 + extension + frames * size
        cursor, end = 28, 28 + extension
        relevant = []
        while cursor < end:
            tag, length = struct.unpack_from('<2I', data, cursor)
            payload = data[cursor+8:cursor+8+length]
            assert len(payload) == length
            if tag in (0x303, 0x80000111):
                relevant.append((tag, hashlib.sha256(payload).hexdigest()))
            cursor += 8 + (length + 3) // 4 * 4
        assert cursor == end and frames == row.get('frames', 600) and relevant
        signature = {'frames': frames, 'record_size': size,
                     'input_sha256': hashlib.sha256(data[end:]).hexdigest(),
                     'guest_state_records': relevant}
        guest.append(signature)
        cadence = (path.parent / 'stdout.log').read_text(encoding='utf-8')
        row['audio_underrun_samples'] = [int(n) for n in re.findall(r'underruns=\+(\d+)', cadence)]
        row['audio_overflow_samples'] = [int(n) for n in re.findall(r'overflows=\+(\d+)', cadence)]
        rows.append(row)
    summary = {'samples': rows, 'identical_guest_state_and_input_records': all(g == guest[0] for g in guest),
               'guest_record': guest[0], 'policies': {}}
    if FAMILY == '784':
        summary['identical_published_image_witnesses'] = all(r['presentation_witnesses'] == rows[0]['presentation_witnesses'] for r in rows)
    if FAMILY == '788':
        summary['identical_snapshot_wire_witnesses'] = all(r['snapshot_witnesses'] == rows[0]['snapshot_witnesses'] for r in rows)
    for policy in ('reference', 'native'):
        subset = [r for r in rows if r['policy'] == policy]
        summary['policies'][policy] = {'samples': len(subset),
            'median_cpu_ms': statistics.median(r['cpu_ms'] for r in subset),
            'cpu_spread_ms': max(r['cpu_ms'] for r in subset)-min(r['cpu_ms'] for r in subset),
            'median_wall_seconds': statistics.median(r['seconds'] for r in subset)}
    (OUT / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k != 'samples'}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('step', choices=['prepare', 'pgo-ready', 'anchor-ready', 'run', 'summarize', 'noop'])
    parser.add_argument('--policy', choices=['reference', 'native', 'train'], default='native')
    parser.add_argument('--sample', type=int, default=1)
    parser.add_argument('--family', choices=['789', '784', '788', '790', 'anchor'], default='789')
    parser.add_argument('--frames', type=int, default=600)
    parser.add_argument('--phase', choices=['baseline', 'generate', 'use'], default='baseline')
    parser.add_argument('--playback', type=Path)
    options = parser.parse_args()
    FAMILY = options.family
    FRAMES = options.frames
    PLAYBACK = options.playback
    OUT = LANE / 'reports' / ('consumer-' + FAMILY)
    if options.step == 'noop': print('No-op: serial hidden/muted consumer A/B, existing title objects retained')
    elif options.step == 'prepare': prepare()
    elif options.step == 'pgo-ready': pgo_ready(options.phase)
    elif options.step == 'anchor-ready': anchor_ready()
    elif options.step == 'summarize': summarize()
    else: run(options.policy, options.sample)
