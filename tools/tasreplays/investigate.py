"""Durable, bounded TAS investigations. See investigation.md for the recipe contract."""
import argparse
import contextlib
import csv
import ctypes
import hashlib
from itertools import zip_longest
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from compare_ram_pages import read_pages, PAGE_BYTES, PAGE_COUNT, RAM_BYTES, page_hash
from observation_evidence import compare_returns
from process_budget import wait_budgeted

HERE = Path(__file__).resolve().parent


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    """Atomic replacement for mutable progress; attempts themselves are never reused."""
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def identity(path):
    path = Path(path).resolve(strict=True)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest)


@contextlib.contextmanager
def lock(path):
    with Path(path).open('a+b') as stream:
        stream.seek(0)
        if os.name == 'nt':
            import msvcrt
            if not stream.read(1):
                stream.write(b'0'); stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            if os.name == 'nt':
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def unlocked(path):
    try:
        with lock(path):
            return True
    except OSError:
        return False


def windows_job(memory_mib):
    """Worker joins before launching children: no uncontained child-start window.

    Keep the handle alive until worker exit; Windows kills all remaining descendants.
    No BREAKAWAY flag is permitted. Fail closed if the host refuses containment.
    """
    from ctypes import wintypes as w
    class Basic(ctypes.Structure):
        _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                    ('flags', w.DWORD), ('min_ws', ctypes.c_size_t), ('max_ws', ctypes.c_size_t),
                    ('active', w.DWORD), ('affinity', ctypes.c_size_t), ('priority', w.DWORD),
                    ('scheduling', w.DWORD)]
    class IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in ('ro', 'wo', 'oo', 'rb', 'wb', 'ob')]
    class Extended(ctypes.Structure):
        _fields_ = [('basic', Basic), ('io', IO), ('process_memory', ctypes.c_size_t),
                    ('job_memory', ctypes.c_size_t), ('peak_process', ctypes.c_size_t),
                    ('peak_job', ctypes.c_size_t)]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
    kernel.CreateJobObjectW.restype = w.HANDLE
    kernel.GetCurrentProcess.restype = w.HANDLE
    kernel.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    job = kernel.CreateJobObjectW(None, None)
    limits = Extended()
    limits.basic.flags = 0x2000 | 0x200  # KILL_ON_JOB_CLOSE | JOB_MEMORY
    limits.job_memory = memory_mib * 1024 * 1024
    if not job or not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not kernel.AssignProcessToJobObject(job, kernel.GetCurrentProcess()):
        raise ctypes.WinError(ctypes.get_last_error())
    return job


def worker(attempt):
    attempt = Path(attempt).resolve()
    with lock(attempt / 'worker.lock'):
        spec = read(attempt / 'command.json')
        result = dict(exit_code=None, stop_reason='worker_error')
        try:
            if os.name != 'nt':
                raise ValueError('command supervision currently requires Windows Job Objects')
            job = windows_job(spec['memory_mib'])
            if identity(spec['argv'][0]) != spec['executable']:
                raise ValueError('command executable changed after launch intent')
            with (attempt / 'stdout.log').open('xb') as out, (attempt / 'stderr.log').open('xb') as err:
                process = subprocess.Popen(spec['argv'], cwd=spec['cwd'], env=spec['env'],
                                           stdout=out, stderr=err, stdin=subprocess.DEVNULL,
                                           creationflags=subprocess.CREATE_NO_WINDOW)
                write(attempt / 'started.json', dict(worker_pid=os.getpid(), child_pid=process.pid,
                                                     time=time.time(), executable=identity(spec['argv'][0])))
                # The worker's Job Object also catches descendants surviving parent exit.
                result = wait_budgeted(process, Path(spec['budget_directory']), spec['timeout'],
                                       spec['max_bytes'], spec['max_files'], interval=0.1,
                                       stop_requested=lambda: (Path(spec['run']) / 'STOP').exists())
        except Exception as error:
            result['error'] = str(error)
        result['finished'] = time.time()
        write(attempt / 'result.json', result)


def expand(value, variables):
    if isinstance(value, str):
        for key, replacement in variables.items():
            value = value.replace('{' + key + '}', str(replacement))
        return value
    if isinstance(value, list):
        return [expand(x, variables) for x in value]
    if isinstance(value, dict):
        return {key: expand(x, variables) for key, x in value.items()}
    return value


def validate(recipe):
    if not isinstance(recipe, dict):
        raise ValueError('recipe must be an object')
    if recipe.get('schema') != 'tas-investigation-v1':
        raise ValueError('unsupported recipe schema')
    if type(recipe.get('expected_returns')) is not int or not 1 <= recipe['expected_returns'] <= 1000000:
        raise ValueError('expected_returns outside 1..1000000')
    if not isinstance(recipe.get('identity'), dict) or not {'input', 'profile', 'executable'} <= recipe['identity'].keys():
        raise ValueError('identity requires input, profile, executable file paths')
    if not all(isinstance(v, str) for v in recipe['identity'].values()):
        raise ValueError('identity values must be file paths')
    phases = ['build', 'test', 'replay', 'snapshots', 'trace']
    previous = -1
    names = set()
    if not isinstance(recipe.get('steps', []), list):
        raise ValueError('steps must be an array')
    for step in recipe.get('steps', []):
        if not isinstance(step, dict) or not isinstance(step.get('id'), str):
            raise ValueError('step requires a string id')
        if not re.fullmatch('[a-z0-9_-]+', step['id']) or step['id'] in names:
            raise ValueError('invalid or duplicate step id')
        names.add(step['id'])
        current = phases.index(step.get('phase'))
        if current < previous:
            raise ValueError('steps must follow build/test/replay/snapshots/trace order')
        previous = current
        if not isinstance(step.get('argv'), list) or not step['argv'] or not all(isinstance(x, str) for x in step['argv']):
            raise ValueError('argv must be a nonempty array of strings')
        if not isinstance(step.get('cwd', ''), str) or not isinstance(step.get('env', {}), dict):
            raise ValueError('cwd must be a string and env an object')
        if not all(isinstance(k, str) and isinstance(v, str) for k, v in step.get('env', {}).items()):
            raise ValueError('environment keys and values must be strings')
        for key, ceiling in [('timeout', 86400), ('max_bytes', 1024**4), ('max_files', 1000000), ('memory_mib', 262144)]:
            value = step.get(key)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 < value <= ceiling:
                raise ValueError('missing or invalid finite budget: ' + key)
            if key != 'timeout' and type(value) is not int:
                raise ValueError(key + ' must be an integer')
        if type(step.get('attempts', 1)) is not int or not 1 <= step.get('attempts', 1) <= 3:
            raise ValueError('attempts outside 1..3')
        if step.get('attempts', 1) > 1 and not any('{attempt}' in x for x in step['argv']):
            raise ValueError('retries require an attempt-local output argument')
    for key in ('comparison', 'snapshots'):
        spec = recipe.get(key, recipe.get('comparison'))
        if not isinstance(spec, dict) or any(not isinstance(spec.get(side), str) or not spec[side] for side in ('source', 'native')):
            raise ValueError(key + ' needs source/native directory strings')
    trace = recipe.get('trace')
    if trace is not None:
        if not isinstance(trace, dict) or type(trace.get('expected_operations')) is not int or not 1 <= trace['expected_operations'] <= 1000000:
            raise ValueError('trace needs expected_operations in 1..1000000')
        if not isinstance(trace.get('columns'), list) or not trace['columns'] or not all(isinstance(c, str) and c for c in trace['columns']):
            raise ValueError('trace needs a nonempty array of column names')
        for side in ('source', 'native'):
            if not isinstance(trace.get(side), dict) or any(not isinstance(trace[side].get(k), str) for k in ('path', 'phase', 'window', 'alignment')):
                raise ValueError('trace sides need path, phase, window and alignment strings')
    if not isinstance(recipe.get('milestones', []), list) or any(not isinstance(m, dict) or
            any(not isinstance(m.get(k), str) or not m[k] for k in ('claim', 'evidence')) for m in recipe.get('milestones', [])):
        raise ValueError('milestones need claim and evidence strings')


def compare_pair(spec, expected):
    """Retain partial coverage even if a later row is malformed; never skip rows."""
    roots = [Path(spec[k]) for k in ('source', 'native')]
    paths = [p / 'ram-pages.tsv' for p in roots]
    before = [identity(p) if p.is_file() else None for p in paths]
    result = dict(match=False, exact_matching_prefix=0, captured_returns=[0, 0],
                  furthest_execution=[0, 0], first_divergence=None, errors=[],
                  scope='all RAM page hashes and return clocks; hashes are not byte equality')
    # ponytail: retain only the first discrepancy, counts and requested raw snapshots;
    # stream million-return indexes instead of materializing their page arrays.
    streams = [iter(read_pages(p)) for p in paths]
    live = [True, True]
    index = 0
    while any(live):
        pair = [None, None]
        for side in range(2):
            if live[side]:
                try:
                    pair[side] = next(streams[side])
                    result['captured_returns'][side] += 1
                    result['furthest_execution'][side] = pair[side][0]
                except StopIteration:
                    live[side] = False
                except (OSError, ValueError) as error:
                    live[side] = False
                    result['errors'].append(str(error))
        if pair == [None, None]:
            break
        index += 1
        a, b = pair
        discrepancy = None
        if a is None or b is None:
            discrepancy = dict(kind='missing_return', frame=index,
                               missing_side='source' if a is None else 'native')
        elif a[1:] != b[1:]:
            discrepancy = dict(kind='state_or_clock', frame=index, source_cycle=a[1], native_cycle=b[1],
                               changed_pages=[f'{p*PAGE_BYTES:06X}' for p in range(PAGE_COUNT) if a[2][p] != b[2][p]])
        if result['first_divergence'] is None:
            if discrepancy:
                result['first_divergence'] = discrepancy
            else:
                result['exact_matching_prefix'] = index
    if result['first_divergence'] is None and min(result['captured_returns']) < expected:
        result['first_divergence'] = dict(kind='missing_return', frame=index + 1, missing_side='both')
    if not result['errors']:
        # Keep the established comparator as the verdict authority on valid captures.
        result.update(compare_returns(*paths, expected))
        if result['first_divergence'] is None and min(result['captured_returns']) < expected:
            result['first_divergence'] = dict(kind='missing_return', frame=index + 1, missing_side='both')
    result['coverage_complete'] = result['captured_returns'] == [expected, expected]
    if max(result['captured_returns']) > expected and (result['first_divergence'] is None or result['first_divergence']['frame'] > expected):
        result['first_divergence'] = dict(kind='unexpected_return', frame=expected + 1)
        result['exact_matching_prefix'] = min(result['exact_matching_prefix'], expected)
    result['evidence'] = [identity(p) for p in paths if p.is_file()]
    if result['evidence'] != [item for item in before if item]:
        result['match'] = False
        result['errors'].append('capture changed during comparison; result is not stable evidence')
    return result


def snapshots(spec, frame):
    result = dict(frame=frame, status='unavailable')
    try:
        roots = [Path(spec[k]) for k in ('source', 'native')]
        data = []
        for root in roots:
            path = root / f'ram-frame-{frame:06d}.bin'
            raw = path.read_bytes()
            if len(raw) != RAM_BYTES:
                raise ValueError('raw snapshot must be exactly 2 MiB')
            row = next((r for r in read_pages(root / 'ram-pages.tsv') if r[0] == frame), None)
            if row is None or any(page_hash(raw[i*PAGE_BYTES:(i+1)*PAGE_BYTES]) != row[2][i] for i in range(PAGE_COUNT)):
                raise ValueError('snapshot disagrees with page index')
            data.append(raw)
        offsets = [i for i, (a, b) in enumerate(zip(*data)) if a != b]
        result.update(status='compared', bytes_differ=len(offsets), first_byte=offsets[0] if offsets else None,
                      evidence=[identity(root / f'ram-frame-{frame:06d}.bin') for root in roots])
    except (OSError, ValueError) as error:
        result['error'] = str(error)
    return result


def trace_pair(spec):
    """Compare only explicitly aligned, same-phase TSV observations; no resync."""
    result = dict(status='unavailable', exact_matching_operations=0, first_difference=None,
                  scope='configured observable columns only; not automatic causal diagnosis')
    if not spec:
        return result
    try:
        sides = [spec[k] for k in ('source', 'native')]
        for key in ('phase', 'window', 'alignment'):
            if not sides[0].get(key) or sides[0][key] != sides[1].get(key):
                raise ValueError('trace observation ' + key + ' differs or is unspecified')
        if not spec.get('columns') or type(spec.get('expected_operations')) is not int or spec['expected_operations'] < 1:
            raise ValueError('trace needs columns and finite expected_operations')
        counts = [0, 0]
        with contextlib.ExitStack() as stack:
            readers = [csv.DictReader(stack.enter_context(Path(s['path']).open(newline='')), delimiter='\t') for s in sides]
            if any(not r.fieldnames or not set(spec['columns']) <= set(r.fieldnames) for r in readers):
                raise ValueError('trace columns missing')
            for index, pair in enumerate(zip_longest(*readers), 1):
                counts = [count + (row is not None) for count, row in zip(counts, pair)]
                values = [None if row is None else [row[c] for c in spec['columns']] for row in pair]
                if any(row is not None and (None in row or any(v is None for v in row.values())) for row in pair):
                    raise ValueError('malformed trace row')
                if result['first_difference'] is None:
                    if values[0] != values[1]:
                        result['first_difference'] = dict(operation=index, source=values[0], native=values[1],
                                                          kind='missing_operation' if None in values else 'observable_operation')
                    else:
                        result['exact_matching_operations'] = index
        result.update(status='compared', captured_operations=counts,
                      coverage_complete=counts == [spec['expected_operations']] * 2,
                      evidence=[identity(s['path']) for s in sides])
    except (OSError, ValueError, KeyError) as error:
        result['error'] = str(error)
    return result


def execute(step, run, variables, state, bindings):
    for number in range(1, step.get('attempts', 1) + 1):
        attempt = run / 'attempts' / f"{step['id']}-{number}"
        if not attempt.exists():
            if (run / 'PAUSE').exists() or (run / 'STOP').exists():
                return 'stopped' if (run / 'STOP').exists() else 'paused'
            for bound in bindings.values():
                if identity(bound['path']) != bound:
                    raise ValueError('bound input/profile/executable changed: ' + bound['path'])
            attempt.mkdir(parents=True)
            spec = expand(step, dict(variables, attempt=attempt))
            if any(re.search(r'\{[a-z_]+\}', arg) for arg in spec['argv']):
                raise ValueError('unknown or unavailable command placeholder')
            spec['cwd'] = str(Path(spec.get('cwd', attempt)).resolve(strict=True))
            executable = shutil.which(spec['argv'][0])
            if not executable:
                raise ValueError('command executable not found: ' + spec['argv'][0])
            spec['argv'][0] = str(Path(executable).resolve())
            spec['env'] = {**{k: v for k, v in os.environ.items()
                              if not SECRET_ENV.match(k)}, **spec.get('env', {})}
            spec.update(run=str(run), budget_directory=str(run), launched=time.time(),
                        executable=identity(spec['argv'][0]))
            write(attempt / 'command.json', spec)  # intent precedes launch; never inferred safe to repeat
            with (attempt / 'worker.log').open('xb') as log:
                subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '_worker', str(attempt)],
                                 stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        spec = read(attempt / 'command.json')
        state.update(status='running', active_step=step['id'], active_attempt=number)
        write(run / 'progress.json', state)
        while not ((attempt / 'result.json').exists() and unlocked(attempt / 'worker.lock')):
            if time.time() > spec['launched'] + spec['timeout'] + 45:
                return 'uncertain'  # Never retry a command without a durable terminal receipt.
            if (run / 'PAUSE').exists() and not (run / 'STOP').exists():
                return 'paused'  # active bounded worker completes; resume collects its result
            if time.time() - state.get('heartbeat', 0) >= 1:
                state['heartbeat'] = time.time()
                write(run / 'progress.json', state)
            time.sleep(0.1)
        outcome = read(attempt / 'result.json')
        state.setdefault('outcomes', {})[attempt.name] = outcome
        write(run / 'progress.json', state)
        if outcome['stop_reason'] == 'operator_stop':
            return 'stopped'
        if outcome['exit_code'] == 0 and outcome['stop_reason'] is None:
            return 'complete'
    return 'failed'


def report(run, state):
    write(run / 'progress.json', state)
    comparison = state.get('comparison', {})
    lines = [f"Investigation: {state['status']}",
             f"Controller error: {state.get('error', 'none')}",
             f"Exact matching RAM/clock prefix: {comparison.get('exact_matching_prefix', 'unknown')}",
             f"Furthest observed return [source, native]: {comparison.get('furthest_execution', 'unknown')}",
             f"First discrepancy: {json.dumps(comparison.get('first_divergence'))}",
             f"Gameplay milestones (external evidence only): {json.dumps(state.get('milestones', [])) or 'unknown'}",
             'Gameplay status: unknown unless established by the separately cited milestones.',
             'Page-hash/clock agreement is not proof of raw RAM equality or gameplay completion.',
             f"Snapshot analysis: {json.dumps(state.get('snapshots', {'status': 'unavailable'}))}",
             f"Trace analysis: {json.dumps(state.get('trace', {'status': 'unavailable'}))}",
             'Commands and outcomes: attempts/*/command.json and result.json; original failures are retained.']
    (run / 'report.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    settled = all((p.parent / 'result.json').exists() and unlocked(p.parent / 'worker.lock')
                  for p in (run / 'attempts').glob('*/command.json'))
    if settled and state['status'] in ('complete', 'mismatch', 'incomplete', 'failed', 'stopped') and not (run / 'evidence.json').exists():
        if not (run / 'conclusion.json').exists():
            write(run / 'conclusion.json', state)
        excluded = {'controller.lock', 'worker.lock', 'progress.json', 'report.txt', 'PAUSE', 'STOP'}
        write(run / 'evidence.json', [identity(p) for p in sorted(run.rglob('*'))
                                     if p.is_file() and p.name not in excluded and not p.name.endswith('.tmp')])


def investigate(recipe_path, run):
    recipe_path, run = Path(recipe_path).resolve(strict=True), Path(run).resolve()
    recipe = read(recipe_path)
    validate(recipe)
    run.mkdir(parents=True, exist_ok=True)
    with lock(run / 'controller.lock'):
        variables = dict(run=run, recipe_dir=recipe_path.parent, tools=HERE, python=sys.executable)
        configured = expand(recipe, variables)
        if (run / 'recipe.json').exists():
            if read(run / 'recipe.json') != recipe:
                raise ValueError('recipe changed; use a new run directory')
            bindings = read(run / 'identity.json')
            variables['recipe_dir'] = read(run / 'context.json')['recipe_dir']
            configured = expand(recipe, variables)
        else:
            if any(p.name != 'controller.lock' for p in run.iterdir()):
                raise ValueError('new run directory must be empty')
            bindings = {name: identity(path) for name, path in configured['identity'].items() if name != 'executable'}
            bindings.update({'tool_' + p.stem: identity(p) for p in
                             [Path(__file__), HERE / 'compare_ram_pages.py', HERE / 'observation_evidence.py', HERE / 'process_budget.py']})
            (run / 'inputs').mkdir()
            for name, bound in bindings.items():
                if not re.fullmatch('[a-zA-Z0-9_-]+', name):
                    raise ValueError('invalid identity name')
                shutil.copyfile(bound['path'], run / 'inputs' / name)
            write(run / 'identity.json', bindings)
            write(run / 'context.json', dict(recipe_dir=str(recipe_path.parent)))
            write(run / 'recipe.json', recipe)
        state = read(run / 'progress.json') if (run / 'progress.json').exists() else dict(status='new', completed=[], milestones=[])
        if state['status'] in ('complete', 'mismatch', 'incomplete', 'failed', 'stopped'):
            report(run, state)
            return state
        try:
            # Reap old workers before checking identities or starting any new phase.
            # An identity failure must not leave a live worker behind a terminal report.
            for command in (run / 'attempts').glob('*/command.json'):
                spec = read(command)
                while not ((command.parent / 'result.json').exists() and unlocked(command.parent / 'worker.lock')):
                    if (run / 'PAUSE').exists() and not (run / 'STOP').exists():
                        state['status'] = 'paused'
                        report(run, state)
                        return state
                    if time.time() > spec['launched'] + spec['timeout'] + 45:
                        state['status'] = 'uncertain'
                        report(run, state)
                        return state
                    time.sleep(.1)
                state.setdefault('outcomes', {})[command.parent.name] = read(command.parent / 'result.json')
            for phase in ('build', 'test', 'replay', 'snapshots', 'trace'):
                if (run / 'STOP').exists() or (run / 'PAUSE').exists():
                    state['status'] = 'stopped' if (run / 'STOP').exists() else 'paused'
                    report(run, state)
                    return state
                if phase == 'replay':
                    if 'executable' not in bindings:
                        bindings['executable'] = identity(configured['identity']['executable'])
                        shutil.copyfile(bindings['executable']['path'], run / 'inputs/executable')
                        write(run / 'identity.json', bindings)
                    for bound in bindings.values():
                        if identity(bound['path']) != bound:
                            raise ValueError('bound identity changed: ' + bound['path'])
                if phase == 'snapshots':
                    if 'comparison' not in state:
                        state['comparison'] = compare_pair(configured['comparison'], recipe['expected_returns'])
                        write(run / 'progress.json', state)
                    first = state['comparison']['first_divergence']
                    if not first or first['kind'] != 'state_or_clock':
                        break
                    variables['first_return'] = first['frame']
                for step in recipe.get('steps', []):
                    if step['phase'] != phase or step['id'] in state['completed']:
                        continue
                    status = execute(step, run, variables, state, bindings)
                    if status != 'complete':
                        state['status'] = status
                        # A process failure still has independently useful partial evidence.
                        if phase in ('replay', 'snapshots', 'trace') and status in ('failed', 'stopped') and 'comparison' not in state:
                            state['comparison'] = compare_pair(configured['comparison'], recipe['expected_returns'])
                        report(run, state)
                        return state
                    state['completed'].append(step['id'])
                    write(run / 'progress.json', state)
                if phase == 'snapshots':
                    spec = expand(recipe.get('snapshots', recipe['comparison']), variables)
                    state['snapshots'] = snapshots(spec, variables['first_return'])
                if phase == 'trace':
                    state['trace'] = trace_pair(expand(recipe.get('trace'), variables))
            for milestone in configured.get('milestones', []):
                state['milestones'].append(dict(claim=milestone['claim'], evidence=identity(milestone['evidence'])))
            c = state['comparison']
            state['status'] = 'complete' if c['match'] else ('mismatch' if c['first_divergence'] and c['first_divergence']['kind'] == 'state_or_clock' else 'incomplete')
        except (OSError, ValueError, KeyError) as error:
            state.update(status='failed', error=str(error))
        report(run, state)
        return state


# command.json is both the retained evidence and the launch contract the worker
# reads back, so the environment cannot simply be masked on write. Agent session
# credentials are dropped from the child environment entirely: no build, test or
# replay step reads them, and a retained run directory is copied to shared storage.
SECRET_ENV = re.compile(r'(?i)^(CLAUDE_CODE_.*|ANTHROPIC_.*|AWS_.*|GH_TOKEN|GITHUB_TOKEN'
                        r'|.*_API_KEY|.*_SECRET|.*_TOKEN|.*_PASSWORD|.*_CREDENTIALS)$')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    start = sub.add_parser('run', help='start or recover the same immutable recipe')
    start.add_argument('recipe', type=Path); start.add_argument('directory', type=Path)
    for action in ('pause', 'resume', 'stop'):
        p = sub.add_parser(action); p.add_argument('directory', type=Path)
    internal = sub.add_parser('_worker', help=argparse.SUPPRESS)
    internal.add_argument('directory', type=Path)
    args = parser.parse_args()
    if args.command == '_worker':
        worker(args.directory); return 0
    if args.command in ('pause', 'stop'):
        if not (args.directory / 'recipe.json').exists():
            parser.error('not an initialized investigation')
        (args.directory / ('PAUSE' if args.command == 'pause' else 'STOP')).touch()
        return 0
    if args.command == 'resume':
        (args.directory / 'PAUSE').unlink(missing_ok=True)
        args.recipe = args.directory / 'recipe.json'
    try:
        state = investigate(args.recipe, args.directory)
        print(json.dumps(dict(status=state['status'], report=str(args.directory / 'report.txt'))))
        return 0 if state['status'] == 'complete' else 2
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr); return 2


if __name__ == '__main__':
    sys.exit(main())
