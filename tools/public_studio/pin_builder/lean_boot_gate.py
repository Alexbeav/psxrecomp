"""Headless moved-product gate, adapted from fanout/headless_boot.ps1.
Caller owns workspace parking, settings, payload selection and promotion.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time

try:
    from . import start_memory
except ImportError:
    try:
        import start_memory
    except ImportError:
        # linux_build and mac_build ship this one file to a Unix test host and run
        # it with the system Python. There is no job object there in any case.
        start_memory = None

try:
    from . import overlay_only_gate
except ImportError:
    try:
        import overlay_only_gate
    except ImportError:
        # The same Unix test hosts: without the gate module beside this file a start
        # row has no `overlay_code_interpreted`. Only the Windows builds put it in a
        # receipt and warn (record_start_findings), and they run from the whole tools folder.
        overlay_only_gate = None


def identity(path):
    path = Path(path)
    # Chunked, not hashlib.file_digest: the gate also runs on the Linux build
    # host, whose system Python (3.9 on Rocky 9) predates file_digest.
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest.hexdigest()}


def overlay_split(loader):
    """(native, interpreted) dispatches above the kernel window, or None.

    From pin H the run report splits the loader's dispatch counters by address:
    `disp_*_kernel` for the kernel window (phys < 0x10000) and `disp_*_overlay`
    for everything above it. Older runtimes report only the totals, in which
    kernel-RAM code that interprets because kernel bless is off (PS1B-306)
    counts as if an overlay had been interpreted (PS1B-323).
    """
    pair = (loader.get('disp_native_overlay'), loader.get('disp_interp_overlay'))
    return pair if all(type(value) is int for value in pair) else None


def kernel_dispatch(report):
    """Kernel-window dispatch counts and kernel-bless state: information, never a gate.

    None for a runtime that does not split its counters.
    """
    loader = report.get('overlay_loader') or {}
    if overlay_split(loader) is None:
        return None
    bless = report.get('kbless') or {}
    return {'native': loader.get('disp_native_kernel'), 'interpreted': loader.get('disp_interp_kernel'),
            'kbless_state': bless.get('state'), 'kbless_native_hits': bless.get('native_hits'),
            'kbless_disabled_by_overflow': bless.get('disabled_by_overflow')}


def above_kernel_dispatch(report):
    """Dispatch counts above the kernel window: information, never a gate. None without the split.

    "Above the kernel window" is boot-executable text as well as overlays, so
    these are not overlay-only counts. Time Crisis on the pin H stack interprets
    201,556 dispatches there in each start and runs none natively, while every
    native loader dispatch it makes is in the kernel window; it plays.
    """
    split = overlay_split(report.get('overlay_loader') or {})
    return None if split is None else {'native': split[0], 'interpreted': split[1]}


def overlay_evidence(report):
    """(native loader dispatches, whether any overlay ran at all) from a last-run report.

    The native counter proves the loader runs compiled code, not interpreted. It
    can only prove that when an overlay ran: a reviewed kit can recompile
    everything its first minute executes (MediEvil's intro and title load no
    overlay), which leaves loads, native and interpreted dispatches all at zero
    while the game plays. Treat that as "not exercised", not as a failure.

    With the split counters, kernel code that interprets is not overlay
    activity (PS1B-323): Alien Trilogy with kernel bless on still interprets
    22,477 kernel dispatches a start and loads no overlay. Only that is taken
    out. The native proof stays the loader's total, as before: the two-way
    split cannot tell overlay code from boot-executable text, and judging
    native dispatch above the kernel window alone would stop titles that pass
    (Time Crisis; Rival Schools' second start has 4 loads and no dispatch there).
    """
    loader = report.get('overlay_loader') or {}
    split = overlay_split(loader)
    counts = [loader.get(k, 0) for k in ('disp_native', 'disp_interp', 'loads')]
    counts = [c if type(c) is int else 0 for c in counts]
    if split is not None:
        counts[1] = split[1]  # interpreted above the kernel window; kernel code is reported, not judged
    return counts[0], any(counts)


def overlay_captured(report, *roots):
    """Whether any overlay code was captured, registered or loaded.

    The loader counts every interpreter fallback in disp_interp, including
    exception returns into boot-executable text the kit did not compile and
    kernel-RAM code. Those are not overlays. An overlay the first start ran
    leaves an overlay_captures.json beside the executable (the autocompile
    input), and a compiled one shows up as registered or loaded (PS1B-266).
    """
    loader = report.get('overlay_loader') or {}
    if any(type(loader.get(k)) is int and loader.get(k) > 0 for k in ('loads', 'registered', 'disp_native')):
        return True
    return any((Path(root) / 'overlay_captures.json').exists() for root in roots)


def frame_count(report):
    return report.get('frame', report.get('frames', report.get('frame_count', 0)))


def game_code_interpreted(report):
    """{dispatches, frames, per_frame, last_epc} when a start interpreted game code, else None.

    Game code here is everything above the kernel window: boot-executable text
    the kit does not compile (exception returns into the middle of a function,
    mostly) and code the game placed in RAM. The count needs the split
    counters (pin H); without them it cannot be told from kernel code, and
    the field is absent. Recorded for every start that has a non-zero count
    (the warning has its own threshold, GAME_CODE_WARN_PER_FRAME),
    whether or not the title has overlays, so that one list names every title
    whose game code is partly interpreted: Time Crisis (201,556 a start, has
    overlays, passes) beside Future Cop (23,927, no overlay). The remedy is kit
    work, compiling those return points; it is tracked per title (PS1B-266).
    """
    split = overlay_split(report.get('overlay_loader') or {})
    if split is None or split[1] <= 0:
        return None
    frames = frame_count(report)
    usable = type(frames) is int and frames > 0
    return {'dispatches': split[1], 'frames': frames if usable else None,
            'per_frame': round(split[1] / frames, 2) if usable else None,
            'last_epc': (report.get('cpu') or {}).get('epc')}


CDROM_FIELDS = ('read_starts', 'silent_reads', 'silent_run_max')
# A game's CD library restarts a read that delivers nothing. One or two silent
# reads in a row are normal (the first read after a Stop is cancelled once while
# the motor spins up); three or more mean a read does not deliver in time
# (docs/RUN_REPORT_CDROM.md in the framework).
CDROM_SILENT_RUN_WARN = 3


def cdrom_reads(report):
    """{read_starts, silent_reads, silent_run_max} from a last-run report, or None.

    From pin H the run report counts the read streams of the run: how many
    started, how many ended before their first sector, and the longest run of
    those in a row. Time Crisis stayed on its logo with 191 in a row before the
    CD motor rule and shows 1 with it. A report without the object (any pin
    before H), or with a value that is not a number, gives None.
    """
    cdrom = report.get('cdrom')
    if not isinstance(cdrom, dict):
        return None
    values = {name: cdrom.get(name) for name in CDROM_FIELDS}
    if any(type(value) is not int for value in values.values()):
        return None
    return values


def cdrom_warning(record):
    """One line for a build's warnings when a start's silent reads ran long, else None."""
    if not record or record['silent_run_max'] < CDROM_SILENT_RUN_WARN:
        return None
    return (f"The second start had {record['silent_run_max']} CD reads in a row that delivered no sector "
            f"({record['silent_reads']} silent of {record['read_starts']} read starts). Up to 2 in a row is "
            "normal; 3 or more means a read does not deliver in time, and tens or hundreds mean the game is "
            "stuck retrying a read. Look at the title before it ships.")


# The warning is for titles whose kit should compile more entry points, not for
# every title that interprets a handful of returns. The rate is a judgment from
# three titles (second starts, 2026-10-01): Time Crisis 21.46 a frame and Future
# Cop 1.92 are on the list, Final Fantasy VII 0.01 is not. Revisit it with the
# pin H wave summaries. wave_summary.py's default --min-rate is the same value.
GAME_CODE_WARN_PER_FRAME = 1.0


def game_code_warning(record):
    """One line for a build's warnings when a start interpreted game code at
    GAME_CODE_WARN_PER_FRAME or more, else None. A record without a rate warns:
    the count cannot be judged."""
    if not record:
        return None
    if record.get('per_frame') is not None and record['per_frame'] < GAME_CODE_WARN_PER_FRAME:
        return None
    rate = f"{record['per_frame']} per frame over {record['frames']} frames" if record.get('per_frame') is not None \
        else 'frame count unknown'
    return (f"The second start interpreted {record['dispatches']} dispatches of game code ({rate}; last "
            f"exception return {record['last_epc']}). The title runs; that code runs slower than compiled code. "
            "The remedy is kit work: compile those return points (PS1B-266).")


# Alex's decision D29 (2026-10-15): the strict overlay-only gate (PS1B-487) judges
# the second start in its per-frame mode and warns, it does not stop, when the
# start interpreted MORE than this many entries of overlay code a frame. On the
# 226 pin H receipts that flags 21 titles (Grandia, Rollcage, Akuji, Gran Turismo,
# Diablo and 16 more); the EU BIOS share of the old-meaning counts is at most 55 a
# frame. The loader-miss reasons of those titles are read in the next trial before
# a stop is decided. The limit was replayed on old-meaning receipts (counts from
# power-on, PS1B-513) and is to be re-derived on post-513 reports.
OVERLAY_WARN_PER_FRAME = 100


def overlay_code_interpreted(report):
    """The overlay-only gate's per-frame verdict on a start (PS1B-487, D29), or None.

    None when the report has no `interp_detail.per_address.by_place` (a runtime
    before pin H: nothing is said and nothing is warned, as before) or when the
    gate module is not beside this file (a Unix test host). Otherwise the record
    is kept for every start, a pass included, so that the limit can be re-derived
    from receipts: `verdict` (pass, fail or refused), `overlay` (the judged
    entries), `frames` (after game entry on a post-513 report), `per_frame`,
    `limit`, `generation`, `lower_bound`, `text` and `kernel` (not judged),
    `reasons` (the loader's misses above the kernel window, nonzero ones; None
    when not reported) and the gate's `line`. A refused record also has `reason`.
    """
    if overlay_only_gate is None or not isinstance(report, dict):
        return None
    detail = report.get('interp_detail')
    table = detail.get('per_address') if isinstance(detail, dict) else None
    if not isinstance(table, dict) or 'by_place' not in table:
        return None
    result = overlay_only_gate.judge(report, OVERLAY_WARN_PER_FRAME, per_frame=True)
    record = {key: result[key] for key in ('verdict', 'overlay', 'frames', 'per_frame', 'limit', 'generation',
                                           'lower_bound', 'text', 'kernel')}
    record['reasons'] = overlay_only_gate.miss_reasons(report)
    if result['verdict'] == overlay_only_gate.REFUSED:
        record['reason'] = result['reason']
    record['line'] = result['line']
    return record


def overlay_code_warning(record):
    """One line for a build's warnings when the second start interpreted overlay code at more than
    OVERLAY_WARN_PER_FRAME entries a frame, else None. A record the gate refused warns: the count
    cannot be judged. A warning, never a stop (D29)."""
    if not record or record.get('verdict') not in ('fail', 'refused'):
        return None
    if record['verdict'] == 'refused':
        return (f"The overlay-only gate could not judge the second start: {record.get('reason')}. Its overlay "
                f"interpretation is not known; the build does not stop for it (PS1B-487, D29). {record['line']}")
    return (f"The second start ran overlay code in the interpreter more than {record['limit']} times a frame. "
            "The title runs; that code runs slower than compiled code. A warning, not a stop (D29): read the "
            f"loader-miss reasons before a stop is decided (PS1B-487). {record['line']}")


def overlay_compile(report):
    """What a start says about its own overlay compiles, or None when its runtime does not report them.

    From pin H batch 2 (B19, PS1B-380) the run report has an `overlay_compile` object: the compile driver's
    result lines summed over the start, every `SHARD FAIL` line counted as it arrived, and per failure class
    the first failure and the compiler's own error line. A report without the object is from an older
    runtime: nothing is said and nothing is warned (an old pass stays a pass).

    Failed units are the larger of `units_failed` and `fail_lines`: a start ends on its exit timer, often in
    the middle of a compile run, and that run has no result line. A unit that fails in two runs of one start
    is counted twice by the runtime; this is a count of failures, not of distinct units.
    """
    data = report.get('overlay_compile')
    if not isinstance(data, dict):
        return None

    def number(key):
        value = data.get(key)
        return value if type(value) is int and value > 0 else 0

    classes = []
    for item in data.get('failure_classes') or []:
        if isinstance(item, dict):
            classes.append({'class': str(item.get('class')), 'count': item.get('count'),
                            'first': str(item.get('first_error') or item.get('first') or '')[:300]})
    record = {'runs': number('runs'), 'runs_failed': number('runs_failed'), 'runs_with_result': number('runs_with_result'),
              'attempted': number('units_attempted'), 'compiled': number('units_compiled'),
              'failed': max(number('units_failed'), number('fail_lines')), 'skipped': number('units_skipped'),
              'classes': classes, 'failures_in_unnamed_classes': number('fail_lines_in_unnamed_classes'),
              'state': data.get('state'), 'consistent': data.get('consistent')}
    if record['runs'] and not record['runs_with_result'] and data.get('state') == 'idle':
        # The driver ran and never printed its result line.
        record['no_result'] = True
        record['output_tail'] = str(data.get('output_tail') or '')[-300:]
    # `runs_failed` also counts a driver that could not be started, so it can be larger than `runs`.
    # That is not an inconsistent report: the difference is the starts of the driver that failed.
    if record['runs_failed'] > record['runs']:
        record['driver_not_started'] = record['runs_failed'] - record['runs']
    return record


def overlay_compile_warning(record, start):
    """One line for a build's warnings when a start's own compiles failed, else None."""
    if not record:
        return None
    if record.get('failed'):
        classes = ', '.join(f"{item.get('count')} {item.get('class')}" for item in record.get('classes') or [])
        first = next((item['first'] for item in record.get('classes') or [] if item.get('first')), '')
        more = (f" {record['failures_in_unnamed_classes']} more failure(s) are in classes the runtime did not name."
                if record.get('failures_in_unnamed_classes') else '')
        return (f"{start} reported {record['failed']} failed overlay compile(s)"
                + (f" ({classes})" if classes else '') + '. Those units run interpreted.'
                + (f" First: {first}" if first else '') + more
                + ' A unit that fails in two compile runs is counted twice (PS1B-380).')
    if record.get('no_result'):
        return (f"{start} ran its overlay compile driver {record['runs']} time(s) and it never printed a result. "
                f"The end of its output: {record.get('output_tail')!r} (PS1B-380).")
    if record.get('driver_not_started'):
        return (f"{start} could not start its overlay compile driver {record['driver_not_started']} time(s); "
                "what it captured then was not compiled (PS1B-380).")
    return None


def start_label(row):
    return f"The start from disc {row['disc']}" if row.get('disc') is not None else f"Start {row.get('number')}"


def record_overlay_compile(rows, receipt):
    """Put every start's own overlay compile results into a build receipt (`overlay_compile_starts`), with
    one warning for the start that reported the most failures. Starts of an older runtime report nothing.
    Nothing here stops a build."""
    starts = [{'start': start_label(row), **row['overlay_compile']} for row in rows if row.get('overlay_compile')]
    if not starts:
        return
    receipt['overlay_compile_starts'] = starts
    worst = max(starts, key=lambda item: (item.get('failed') or 0, bool(item.get('no_result')),
                                          item.get('driver_not_started') or 0))
    line = overlay_compile_warning(worst, worst['start'])
    if line:
        others = [item['start'] for item in starts if item is not worst and item.get('failed')]
        receipt.setdefault('warnings', []).append(line + (f" Also failing: {', '.join(others)}." if others else ''))


def start_findings(report):
    """What a start's report says that a wave summary lists; only the fields the report has.

      game_code_interpreted     game code that ran interpreted (PS1B-266)
      overlay_code_interpreted  the overlay-only gate's per-frame verdict (PS1B-487, D29; pin H and later)
      cdrom_reads               CD read streams of the run
      overlay_compile           the start's own overlay compile results (PS1B-380; pin H batch 2 and later)
    """
    found = {'game_code_interpreted': game_code_interpreted(report),
             'overlay_code_interpreted': overlay_code_interpreted(report), 'cdrom_reads': cdrom_reads(report),
             'overlay_compile': overlay_compile(report)}
    return {name: value for name, value in found.items() if value is not None}


def record_start_findings(row, receipt):
    """Copy the second start's findings from its gate row (or from start_findings())
    into a build receipt, each with its warning. Nothing here stops a build."""
    interpreted = row.get('game_code_interpreted')
    if interpreted is not None:
        receipt['game_code_interpreted'] = interpreted
        warning = game_code_warning(interpreted)
        if warning:
            receipt.setdefault('warnings', []).append(warning)
    overlay = row.get('overlay_code_interpreted')
    if overlay is not None:
        receipt['overlay_code_interpreted'] = overlay
        warning = overlay_code_warning(overlay)
        if warning:
            receipt.setdefault('warnings', []).append(warning)
    reads = row.get('cdrom_reads')
    if reads is not None:
        receipt['cdrom_reads'] = reads
        warning = cdrom_warning(reads)
        if warning:
            receipt.setdefault('warnings', []).append(warning)


COMPILER_STOP = ('Second start must report frames > 0, and overlay_loader.disp_native > 0 '
                 'whenever an overlay was loaded or interpreted.')


def judge_second_start(report, captured):
    """The stop text for the second start's report, or None when it passes.

    `captured` is whether either start captured, registered or loaded an overlay.
      * No frames: stop.
      * Overlays present and no native loader dispatch: the overlay compiler or
        loader failed. Stop. This rule did not change.
      * No overlay anywhere, and code was interpreted:
          - a report without the split counters cannot tell kernel code from
            game code: stop, with the PS1B-266 text, as before;
          - a split report: pass. Kernel interpretation is not a build matter
            (PS1B-323), and interpreted game code is recorded as
            `game_code_interpreted` and listed, not stopped. The stop fired only
            for titles with no native loader dispatch at all (Future Cop) and
            let through titles that interpret far more (Time Crisis), so it did
            not say what it meant to say (decision 2026-10-01).
    """
    frames = frame_count(report)
    if type(frames) is not int or frames <= 0:
        return COMPILER_STOP
    native, exercised = overlay_evidence(report)
    if not exercised or native > 0:
        return None
    if captured:
        return COMPILER_STOP
    if overlay_split(report.get('overlay_loader') or {}) is not None:
        return None
    return outside_overlay_message(report)


def outside_overlay_message(report):
    """The stop for interpreted code when no overlay was captured or loaded (reports without the split)."""
    loader = report.get('overlay_loader') or {}
    epc = (report.get('cpu') or {}).get('epc')
    interpreted = loader.get('disp_interp')
    return (f'Second start interpreted {interpreted} dispatches, but neither start captured, registered or '
            'loaded an overlay. The interpreted code is outside any overlay: boot-executable or kernel code '
            f'the kit does not compile (last exception return {epc}), or the game never reached the point '
            'where it loads overlays. This is not an overlay compiler failure; review the kit.')


def overlay_cache_progress(*roots):
    """Count compiled overlay artifacts, to tell slow work apart from a wedge.

    A first boot compiles overlay fragments into native DLLs before it can run
    frames. That is legitimate build work happening at launch, and with several
    builds sharing the machine it can exceed any fixed timer. Counting the cache
    lets the gate wait while it is genuinely progressing.
    """
    found = {}
    for root in roots:
        root = Path(root)
        if not root.is_dir():
            continue
        for pattern in ('**/cache/**/*.dll', '**/overlay*/**/*.dll',
                        '**/cache/**/*.so', '**/overlay*/**/*.so'):
            for path in root.glob(pattern):
                try:
                    found[str(path)] = path.stat().st_mtime
                except OSError:
                    continue
    return len(found), (max(found.values()) if found else 0.0)


class MemoryLimitReached(Exception):
    """A process of the start was refused memory at the start's limit (PS1B-352)."""


def start_guard(child):
    """The memory guard of a start just launched, or None where the gate runs without one."""
    return start_memory.guard(child) if start_memory is not None else None


def finish_guard(memory, row):
    """Write the start's `memory` row (limit, peaks, whether the limit was reached) and release the guard."""
    if memory is None:
        row['memory'] = {'limit_mb': None,
                         'not_limited': 'this host runs the gate without its memory module; nothing limits the start'}
        return
    memory.close()  # keeps its last answers
    record = memory.record()
    if record is not None:
        row['memory'] = record


def memory_stop(row, what):
    """Raise the stop for a start whose process reached the memory limit; otherwise return."""
    record = row.get('memory') or {}
    if record.get('exceeded'):
        raise RuntimeError(f'{what} was stopped: ' + start_memory.stop_text(record))


# A start whose heaviest process used this share of the limit gets a warning: it
# is the next one to reach it.
START_MEMORY_LOOK_FRACTION = 0.5


def start_memory_summary(rows):
    """Over the starts of one gate: {limit_mb, peak_process_mb, peak_job_mb, heaviest_start, starts}, or None.

    `rows` are gate rows; the ones without a `memory` row (a pin H gate before
    this, a test's stand-in process) are left out. `not_limited` carries the
    reason when a start ran without a limit.
    """
    measured = [(row, row['memory']) for row in rows if isinstance(row.get('memory'), dict)]
    if not measured:
        return None
    summary = {'limit_mb': measured[0][1].get('limit_mb'), 'starts': len(measured)}
    peaks = [(memory['peak_process_mb'], row) for row, memory in measured if memory.get('peak_process_mb') is not None]
    if peaks:
        heaviest, row = max(peaks, key=lambda item: item[0])
        summary.update(peak_process_mb=heaviest,
                       peak_job_mb=max(memory.get('peak_job_mb') or 0 for _row, memory in measured),
                       heaviest_start=row.get('program') or row.get('disc') or row.get('number'))
    reason = next((memory['not_limited'] for _row, memory in measured if memory.get('not_limited')), None)
    if reason:
        summary['not_limited'] = reason
    return summary


def record_start_memory(rows, receipt):
    """Put the gate's memory summary into a build receipt, with its warnings. Never a stop."""
    summary = start_memory_summary(rows)
    if summary is None:
        return
    receipt['start_memory'] = summary
    warnings = receipt.setdefault('warnings', [])
    if summary.get('not_limited'):
        warnings.append(f"The headless starts ran without a memory limit: {summary['not_limited']}.")
    limit, peak = summary.get('limit_mb'), summary.get('peak_process_mb')
    if limit and peak is not None and peak >= limit * START_MEMORY_LOOK_FRACTION:
        warnings.append(
            f"A process of start {summary['heaviest_start']} used {peak:,.0f} MB, more than half of the "
            f"{limit:,} MB a start may use. Look at the title before it reaches the limit (PS1B-352).")


def wait_with_overlay_progress(child, budget, product, unrelated_cwd, row,
                               idle_grace=120.0, poll=2.0, margin=90.0, memory=None):
    """Wait for the child's own exit timer, and longer while overlays compile.

    Returns the exit code, or raises subprocess.TimeoutExpired. `budget` is the
    child's PSX_EXIT_AFTER_MS timer, so the child cannot exit before it; startup
    and shutdown come on top, hence `margin`. Past that deadline the wait goes
    on only while the overlay cache grew within `idle_grace`. An idle cache
    before the deadline is normal (a game with nothing left to compile) and
    must not stop a healthy run, which is what failed Diablo at 152 s.

    `memory` is the start's guard: the wait ends with MemoryLimitReached as soon
    as the job says a process was refused memory at the limit.
    """
    deadline = time.monotonic() + budget + margin
    last_count, last_newest = overlay_cache_progress(product, unrelated_cwd)
    idle_since = time.monotonic()
    extensions = 0
    while True:
        try:
            if child.wait(timeout=poll) is not None:
                row['overlay_cache'] = {'artifacts': last_count, 'extensions': extensions}
                return child.returncode
        except subprocess.TimeoutExpired:
            pass
        if memory is not None and memory.exceeded():
            raise MemoryLimitReached()
        count, newest = overlay_cache_progress(product, unrelated_cwd)
        if count > last_count or newest > last_newest:
            last_count, last_newest = max(count, last_count), max(newest, last_newest)
            idle_since = time.monotonic()
            if time.monotonic() >= deadline:
                extensions += 1
        if time.monotonic() >= deadline and time.monotonic() - idle_since > idle_grace:
            raise subprocess.TimeoutExpired(child.args if hasattr(child, 'args') else 'boot', budget)


def processes_under(folder):
    """[(pid, image path)] of every running process whose executable is inside `folder`.

    Standard library only, and Python 3.9 syntax: the gate also runs on the
    Linux build host. A process that cannot be opened is skipped.
    """
    root = os.path.normcase(str(Path(folder).resolve())) + os.sep
    found = []
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes

        class Entry(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
                        ('th32ProcessID', wintypes.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                        ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                        ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', ctypes.c_long),
                        ('dwFlags', wintypes.DWORD), ('szExeFile', ctypes.c_wchar * 260)]

        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                      ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        snapshot = kernel.CreateToolhelp32Snapshot(0x00000002, 0)  # TH32CS_SNAPPROCESS
        if snapshot in (None, wintypes.HANDLE(-1).value):
            return found
        try:
            entry = Entry()
            entry.dwSize = ctypes.sizeof(Entry)
            more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while more:
                handle = kernel.OpenProcess(0x1000, False, entry.th32ProcessID)  # QUERY_LIMITED_INFORMATION
                if handle:
                    try:
                        size = wintypes.DWORD(32768)
                        buffer = ctypes.create_unicode_buffer(size.value)
                        if kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                            if os.path.normcase(buffer.value).startswith(root):
                                found.append((int(entry.th32ProcessID), buffer.value))
                    finally:
                        kernel.CloseHandle(handle)
                more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            kernel.CloseHandle(snapshot)
    else:
        for entry in Path('/proc').glob('[0-9]*'):
            try:
                image = os.readlink(str(entry / 'exe'))
            except OSError:
                continue
            if os.path.normcase(image).startswith(root):
                found.append((int(entry.name), image))
    return found


def settle_product_processes(product, timeout=30.0, poll=0.5, lister=processes_under,
                             clock=time.monotonic, sleep=time.sleep, kill=None):
    """After the game exits, wait until nothing still runs from the product folder.

    The game ends its overlay compiler (the bundled Python under
    overlay_toolchain) on exit, but that child, and the byte-range lock it
    holds on the cache's capacity lock file, can outlive the game for a moment
    on a loaded host. Finalize then failed to move the cache: WinError 33 or 5
    (PS1B-296). Wait for those processes; end any that outlast `timeout`.
    """
    kill = kill or (lambda pid: os.kill(pid, signal.SIGTERM))
    started = clock()
    seen = {}
    left = lister(product)
    while left and clock() - started < timeout:
        seen.update(left)
        sleep(poll)
        left = lister(product)
    killed = []
    for pid, image in left:
        seen[pid] = image
        try:
            kill(pid)
            killed.append(pid)
        except OSError:
            pass
    if not seen:
        return None
    return {'waited_seconds': round(clock() - started, 1),
            'processes': [{'pid': pid, 'image': image} for pid, image in sorted(seen.items())],
            'ended_by_gate': killed}


FORCED_CONTROLS = ('PSX_EXIT_HALT', 'PSX_EXIT_AFTER_FRAMES', 'PSX_STOP_AFTER_FRAME',
                   'PSX_STOP_AFTER_FRAMES', 'PSX_MAX_FRAMES', 'PSX_FRAME_LIMIT',
                   'PSX_OVERLAY_AUTOCOMPILE_CMD', 'PSX_OVERLAY_BACKEND')
DISC_START_MILLISECONDS = 60000


def select_disc(settings_text, index):
    """settings.toml text with `[disc] selected = index`, as the launcher's Disc Selection writes it.

    Plain text edit, Python 3.9: the gate also runs where tomllib does not exist.
    Every other line is kept.
    """
    out, table, placed = [], '', False
    for line in settings_text.splitlines():
        stripped = line.strip()
        if stripped.startswith('[') and stripped.endswith(']'):
            table = stripped
            out.append(line)
            if table == '[disc]' and not placed:
                out.append(f'selected = {index}')
                placed = True
            continue
        if table == '[disc]' and re.match(r'\s*selected\s*=', line):
            continue  # the previous selection
        out.append(line)
    if not placed:
        # At the end: a table opened at the top would swallow top-level keys.
        out += ['[disc]', f'selected = {index}']
    return '\n'.join(out) + '\n'


def boot_name_serial(name):
    """A boot file name in the form a roster declares: SCUS_941.63 -> SCUS-94163, anything else upper-cased.

    The same rule as pin_build.normalize_boot_serial, which writes the roster; kept here because this
    file runs alone on the test hosts.
    """
    stem = name.replace('\\', '/').split('/')[-1].split(';')[0].strip()
    found = re.match(r'([A-Za-z]{4})_(\d{3})\.(\d{2})$', stem)
    return f'{found.group(1)}-{found.group(2)}{found.group(3)}'.upper() if found else stem.upper()


def mounted_serial(log_text):
    """The serial of the disc the runtime mounted, from its start-up log, or None.

    The runtime prints `disc region R (serial S)` when it can turn the disc's boot file name into a
    serial with a known region. For a name it cannot (Dragon Warrior VII: SLUSP012.06 and SLUSP013.46)
    it prints no such line, and the roster holds the name as it is on the disc. The boot file name the
    runtime read from the mounted disc is then taken from its text image guard line. That line names
    the disc only when the guard was armed from the disc; armed from a local executable, it does not.
    """
    found = re.search(r'disc region \S+ \(serial ([^)]+)\)', log_text)
    if found:
        return found.group(1)
    found = re.search(r'text image guard armed \(0x[0-9A-Fa-f]+\.\.0x[0-9A-Fa-f]+, disc ([^)]+)\)', log_text)
    return boot_name_serial(found.group(1)) if found else None


def start_other_discs(product, executable, evidence, unrelated_cwd, env, discs, on_run=None, first_number=3,
                      rows=None, kind='disc'):
    """Start the product once per further disc of its roster and judge each start.

    The two standard starts use the remembered disc, which is disc 1. A build of a
    multi-disc set must also start from every other disc: the player can pick any
    of them in the launcher. For disc N the gate writes `[disc] selected = N` into
    the product's settings.toml, which is what the launcher writes, starts the
    product for 60 s, and restores settings.toml afterwards, so the delivered
    folder is unchanged.

    `discs` is a list of {'disc': N, 'serial': expected serial or None}. An entry
    may carry 'select': None to start without touching settings.toml; 'disc' is
    then only a label. That is how a program set starts each of its programs
    from one shared folder (`kind` names what the label is, for file names and
    messages).
    A start passes when it exits 0 on its own timer, leaves no crash file, runs
    frames, and (when a serial is expected) mounts that serial. The overlay rule
    of the second standard start is not applied: a disc's first start may meet
    overlay code for the first time. Its counters are recorded.

    Rows are appended to `rows` as each start begins, so a failed start is still
    in the caller's receipt.
    """
    rows = [] if rows is None else rows
    settings = product / 'settings.toml'
    original = settings.read_bytes() if settings.is_file() else None
    touched = False
    try:
        for offset, disc in enumerate(discs):
            number, index, expected = first_number + offset, disc['disc'], disc.get('serial')
            select = disc.get('select', index)
            for directory in (unrelated_cwd, product):
                for name in ('psx_last_run_report.json', 'psx_crash.txt', 'psx_freeze_heartbeat.json'):
                    (directory / name).unlink(missing_ok=True)
            if select is not None:
                touched = True
                settings.write_bytes(
                    select_disc(original.decode('utf-8-sig') if original else '', int(select)).encode('utf-8'))
            launch_env = env.copy()
            removed = {key: launch_env.pop(key) for key in FORCED_CONTROLS if key in launch_env}
            forced = {'PSX_HEADLESS': '1', 'PSX_NO_LAUNCHER': '1', 'PSX_HOST_MUTE': '1',
                      'PSX_EXIT_AFTER_MS': str(DISC_START_MILLISECONDS), 'SDL_AUDIODRIVER': 'dummy',
                      'SDL_VIDEODRIVER': 'windows' if os.name == 'nt' else 'dummy'}
            launch_env.update(forced)
            row = {'number': number, 'disc': index, 'expected_serial': expected, 'environment': forced,
                   'removed_controls': removed, 'stdout': str(evidence / f'run-{kind}-{index}.out'),
                   'stderr': str(evidence / f'run-{kind}-{index}.err')}
            rows.append(row)
            if on_run:
                on_run(row, 'running')
            start = time.monotonic()
            with open(row['stdout'], 'wb') as stdout, open(row['stderr'], 'wb') as stderr:
                child = subprocess.Popen([str(executable)], cwd=unrelated_cwd, env=launch_env,
                    stdout=stdout, stderr=stderr,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                row['pid'] = child.pid
                memory = start_guard(child)
                try:
                    row['exit_code'] = wait_with_overlay_progress(
                        child, DISC_START_MILLISECONDS / 1000, product, unrelated_cwd, row, memory=memory)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                    raise RuntimeError(f'Headless start of {kind} {index} timed out; the owned process was stopped.')
                except MemoryLimitReached:
                    memory.end()
                    row['exit_code'] = child.wait()
                finally:
                    row['elapsed_seconds'] = round(time.monotonic() - start, 3)
                    settled = settle_product_processes(product)
                    if settled:
                        row['settled_processes'] = settled
                    finish_guard(memory, row)
            memory_stop(row, f'Headless start of {kind} {index}')
            log_text = ''.join(Path(row[name]).read_text(encoding='utf-8', errors='replace')
                               for name in ('stdout', 'stderr'))
            row['mounted_serial'] = mounted_serial(log_text)
            report = unrelated_cwd / 'psx_last_run_report.json'
            if report.is_file():
                saved = evidence / f'last-run-{kind}-{index}.json'
                shutil.copy2(report, saved)
                row['report'] = identity(saved)
            if row['exit_code'] != 0 or any((d / 'psx_crash.txt').exists() for d in (unrelated_cwd, product)):
                raise RuntimeError(f'Headless start of {kind} {index} did not exit cleanly; read its external logs.')
            if 'report' not in row:
                raise RuntimeError(f'Headless start of {kind} {index} produced no fresh last-run report.')
            data = json.loads(saved.read_text(encoding='utf-8-sig'))
            if data.get('exit_origin') != 'exit_after_ms' or row['elapsed_seconds'] < DISC_START_MILLISECONDS / 1000:
                raise RuntimeError(f'Headless start of {kind} {index} exited before its requested clean timer.')
            frames = data.get('frame', data.get('frames', data.get('frame_count', 0)))
            native, exercised = overlay_evidence(data)
            row.update(frames=frames, disp_native=native, overlays_exercised=exercised,
                       overlays_captured=overlay_captured(data, product, unrelated_cwd))
            kernel = kernel_dispatch(data)
            if kernel is not None:
                row['kernel_dispatch'] = kernel
                row['above_kernel_dispatch'] = above_kernel_dispatch(data)
            row.update(start_findings(data))
            if type(frames) is not int or frames <= 0:
                raise RuntimeError(f'Headless start of {kind} {index} ran no frames.')
            if expected and row['mounted_serial'] != expected:
                raise RuntimeError(
                    f'Headless start of {kind} {index} mounted {row["mounted_serial"] or "no named disc"}, '
                    f'not {expected}. The product does not start from that disc of its roster.')
            row['passed'] = True
            if on_run:
                on_run(row, 'done')
    finally:
        if touched and original is None:
            settings.unlink(missing_ok=True)
        elif touched:
            settings.write_bytes(original)
    return rows


def run_lean_boots(product, exe_name, evidence, unrelated_cwd, env, on_run=None, discs=None):
    """Two standard starts, then one start per disc in `discs` (see start_other_discs)."""
    product, evidence, unrelated_cwd = map(lambda p: Path(p).resolve(), (product, evidence, unrelated_cwd))
    if evidence.is_relative_to(product) or unrelated_cwd.is_relative_to(product):
        raise ValueError('Evidence and launch working directory must be outside the staged product.')
    if Path(exe_name).name != exe_name:
        raise ValueError('Executable must be a filename within the staged product.')
    executable = product / exe_name
    if not executable.is_file() or not unrelated_cwd.is_dir():
        raise ValueError('Staged executable and unrelated working directory must exist.')
    evidence.mkdir(parents=True, exist_ok=False)
    result = {'schema': 1, 'status': 'running', 'executable': identity(executable),
              'cwd': str(unrelated_cwd), 'argv': [str(executable)], 'runs': []}
    receipt = evidence / 'lean-boot-receipt.json'
    try:
        # A first boot compiles overlays before it can run frames, so the clean
        # exit timer and the harness budget must both leave room for it. The
        # previous 90s timer with a 30s grace reported a slow first boot as a
        # failure rather than waiting for the process's own clean exit.
        for number, milliseconds in ((1, 180000), (2, 60000)):
            # crash_trace writes relative to process cwd, not the executable.
            # Both locations are caller-owned; stale diagnostics cannot pass.
            for directory in (unrelated_cwd, product):
                for name in ('psx_last_run_report.json', 'psx_crash.txt', 'psx_freeze_heartbeat.json'):
                    (directory / name).unlink(missing_ok=True)
            launch_env = env.copy()
            removed = {}
            for key in ('PSX_EXIT_HALT', 'PSX_EXIT_AFTER_FRAMES', 'PSX_STOP_AFTER_FRAME',
                        'PSX_STOP_AFTER_FRAMES', 'PSX_MAX_FRAMES', 'PSX_FRAME_LIMIT',
                        'PSX_OVERLAY_AUTOCOMPILE_CMD', 'PSX_OVERLAY_BACKEND'):
                if key in launch_env:
                    removed[key] = launch_env.pop(key)
            forced = {'PSX_HEADLESS': '1', 'PSX_NO_LAUNCHER': '1', 'PSX_HOST_MUTE': '1',
                      'PSX_EXIT_AFTER_MS': str(milliseconds), 'SDL_AUDIODRIVER': 'dummy',
                      # Headless mode opens no window; Linux has no 'windows' driver.
                      'SDL_VIDEODRIVER': 'windows' if os.name == 'nt' else 'dummy'}
            launch_env.update(forced)
            row = {'number': number, 'environment': forced, 'removed_controls': removed,
                   'stdout': str(evidence / f'run-{number}.out'), 'stderr': str(evidence / f'run-{number}.err')}
            result['runs'].append(row)
            if on_run:
                on_run(row, 'running')
            start = time.monotonic()
            with open(row['stdout'], 'wb') as stdout, open(row['stderr'], 'wb') as stderr:
                child = subprocess.Popen([str(executable)], cwd=unrelated_cwd, env=launch_env,
                    stdout=stdout, stderr=stderr,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                row['pid'] = child.pid
                memory = start_guard(child)
                try:
                    row['exit_code'] = wait_with_overlay_progress(
                        child, milliseconds / 1000, product, unrelated_cwd, row, memory=memory)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                    raise RuntimeError(f'Headless run {number} timed out; the owned process was stopped.')
                except MemoryLimitReached:
                    memory.end()
                    row['exit_code'] = child.wait()
                finally:
                    row['elapsed_seconds'] = round(time.monotonic() - start, 3)
                    settled = settle_product_processes(product)
                    if settled:
                        row['settled_processes'] = settled
                    finish_guard(memory, row)
            memory_stop(row, f'Headless run {number}')
            report = unrelated_cwd / 'psx_last_run_report.json'
            if report.is_file():
                saved = evidence / f'last-run-{number}.json'
                shutil.copy2(report, saved)
                row['report'] = identity(saved)
            if row['exit_code'] != 0 or any((d / 'psx_crash.txt').exists() for d in (unrelated_cwd, product)):
                raise RuntimeError(f'Headless run {number} did not exit cleanly; read its external logs.')
            if 'report' not in row:
                raise RuntimeError(f'Headless run {number} produced no fresh last-run report.')
            data = json.loads(saved.read_text(encoding='utf-8-sig'))
            if data.get('exit_origin') != 'exit_after_ms' or row['elapsed_seconds'] < milliseconds / 1000:
                raise RuntimeError(f'Headless run {number} exited before its requested clean timer.')
            frames = frame_count(data)
            native, exercised = overlay_evidence(data)
            captured = overlay_captured(data, product, unrelated_cwd)
            row.update(frames=frames, disp_native=native, overlays_exercised=exercised, overlays_captured=captured)
            kernel = kernel_dispatch(data)
            if kernel is not None:
                row['kernel_dispatch'] = kernel
                row['above_kernel_dispatch'] = above_kernel_dispatch(data)
            row.update(start_findings(data))
            if number == 2:
                # "Either start": the first start's counters count as well as its capture file.
                stop = judge_second_start(data, captured or any(r.get('overlays_captured') for r in result['runs']))
                if stop is not None:
                    raise RuntimeError(stop)
            if on_run:
                on_run(row, 'done')
        if discs:
            # Kept apart from `runs`: readers of the receipt take the second
            # standard start as runs[-1].
            result['disc_runs'] = []
            start_other_discs(product, executable, evidence, unrelated_cwd, env, discs,
                              on_run=on_run, rows=result['disc_runs'])
        if identity(executable) != result['executable']:
            raise RuntimeError('Staged executable changed during the boot gate.')
        result['status'] = 'passed'
        return result
    except Exception as error:
        result.update(status='failed', error=str(error))
        raise
    finally:
        receipt.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
