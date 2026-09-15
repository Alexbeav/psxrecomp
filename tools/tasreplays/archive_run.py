"""Archive one TAS replay run directory into a receipt-driven evidence archive.

Scope: this tool copies evidence and drafts notes. It reads the run's own
receipts (manifest.json, exit.json, the title receipt, the sibling ladder
receipt and the project setup receipt), copies the run byte-for-byte into
<evidence-root>/<archive-name>/runs/<run>/, verifies every copied file by
SHA-256, appends to the archive's SHA256SUMS.txt without ever rewriting an
existing hash, writes <run>.archive.json, and renders a mailbox note plus a
one-paragraph board amendment from the receipt fields. It does not qualify
anything: the verdict it reports is the receipt's verdict, restated.

Usage:
  python archive_run.py <run-dir> <archive-name> [--evidence-root DIR]
      [--author pegasus-claude] [--to "eagle-codex, eagle-claude"]
      [--cc operator] [--task TEXT] [--mailbox DIR] [--dry-run] [--label TEXT]

Exit codes: 0 success, 1 verification failure, 2 usage error.
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

DEFAULT_EVIDENCE_ROOT = r'Z:\Share\psxrecomp\tas-evidence'
SUMS_NAME = 'SHA256SUMS.txt'
TEKKEN_RETURNS = 8399
HOST_LIMITS = {'host_timeout', 'host_storage_budget'}
TITLES = {
    'biohazard': {'name': 'Bio Hazard', 'receipt': 'source-comparison.json', 'exe': 'biohazard-tas.exe',
                  'schema': 'biohazard-tas-candidate-v1'},
    'megamanx5': {'name': 'Mega Man X5', 'receipt': 'source-comparison.json', 'exe': 'megamanx5-tas.exe',
                  'schema': 'megamanx5-tas-candidate-v1'},
    'tekken3': {'name': 'Tekken 3', 'receipt': 'verification.json', 'exe': 'tekken3-tas.exe',
                'schema': 'psx-tas-setup-v1'},
    'pepsiman': {'name': 'Pepsiman', 'receipt': 'verification.json', 'exe': 'pepsiman-tas.exe',
                 'schema': 'pepsiman-tas-candidate-v1'},
}
BUILD_FILES = ('setup.json', 'game.toml', 'bios.toml', 'input.json')
SIBLING_SUFFIXES = ('-ladder.json', '-input.psxrti2', '-input.psxrti')


class ArchiveError(Exception):
    """A verification failure: nothing further may be trusted or written."""


class UsageError(Exception):
    """Bad arguments or an unusable run directory."""


# ----------------------------------------------------------------- helpers

def sha256_file(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sha256_text(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def load_json(path):
    """Parse a JSON file, or return None when it is absent or unreadable."""
    path = Path(path)
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return None


def walk_files(root):
    """All files below root as (relative posix path, absolute Path), sorted."""
    root = Path(root)
    found = []
    for directory, _dirs, names in os.walk(root):
        for name in names:
            path = Path(directory) / name
            found.append((path.relative_to(root).as_posix(), path))
    return sorted(found)


def short(sha, n=12):
    return sha[:n] if isinstance(sha, str) else 'unknown'


def fmt_int(value):
    return f'{value:,}' if isinstance(value, int) and not isinstance(value, bool) else str(value)


def local_now():
    return datetime.datetime.now().astimezone()


# ------------------------------------------------------------ run loading

def _staged_exe_name(manifest):
    """Lowercased file name of the executable the run staged, or ''."""
    try:
        return Path(manifest['inputs']['exe']['path']).name.lower()
    except (KeyError, TypeError):
        pass
    if isinstance(manifest.get('command'), list) and manifest['command']:
        return Path(str(manifest['command'][0])).name.lower()
    return ''


def detect_title(manifest, receipts, setup):
    """Title key from the receipt present, then the exe name, then setup schema."""
    if 'source-comparison.json' in receipts:
        # More than one title writes this receipt name, so its presence alone
        # does not identify the title -- it once labelled every Mega Man X5
        # archive "Bio Hazard". Disambiguate on the setup schema, then the
        # staged executable, and only then fall back.
        sharing = [(k, s) for k, s in TITLES.items() if s['receipt'] == 'source-comparison.json']
        schema = (setup or {}).get('schema')
        for key, spec in sharing:
            if schema == spec['schema']:
                return key
        exe = _staged_exe_name(manifest)
        for key, spec in sharing:
            if exe == spec['exe']:
                return key
        return 'biohazard'
    verification = receipts.get('verification.json')
    if verification is not None:
        if 'captured_returns' in verification or 'source_observation_end' in verification:
            return 'pepsiman'
        if 'expected_victory_time' in verification or verification.get('original_inputs') == 7974:
            return 'tekken3'
    exe = _staged_exe_name(manifest)
    for key, spec in TITLES.items():
        if exe == spec['exe']:
            return key
    schema = (setup or {}).get('schema')
    for key, spec in TITLES.items():
        if schema == spec['schema']:
            return key
    if verification is not None:
        return 'tekken3' if 'compared_returns' in verification else 'unknown'
    return 'unknown'


def load_run(run_dir):
    """Collect everything the summary reads. Only manifest.json and exit.json are required."""
    run_dir = Path(run_dir).absolute()
    if not run_dir.is_dir():
        raise UsageError(f'run directory does not exist: {run_dir}')
    manifest = load_json(run_dir / 'manifest.json')
    exit_info = load_json(run_dir / 'exit.json')
    if manifest is None or exit_info is None:
        raise UsageError(f'run directory must contain manifest.json and exit.json: {run_dir}')
    receipts = {}
    for name in ('source-comparison.json', 'verification.json'):
        data = load_json(run_dir / name)
        if data is not None:
            receipts[name] = data
    siblings = {}
    for suffix in SIBLING_SUFFIXES:
        path = run_dir.parent / (run_dir.name + suffix)
        if path.is_file():
            siblings[suffix] = path
    project = None
    setup = None
    try:
        project = Path(manifest['inputs']['game']['path']).parent
    except (KeyError, TypeError):
        pass
    if project is not None:
        setup = load_json(project / 'setup.json')
    title = detect_title(manifest, receipts, setup)
    receipt_name = TITLES.get(title, {}).get('receipt')
    if receipt_name not in receipts:
        receipt_name = next(iter(receipts), None)
    return {
        'run_dir': run_dir, 'manifest': manifest, 'exit': exit_info,
        'complete': load_json(run_dir / 'complete.json'), 'receipts': receipts,
        'receipt_name': receipt_name, 'receipt': receipts.get(receipt_name),
        'ladder': load_json(siblings['-ladder.json']) if '-ladder.json' in siblings else None,
        'siblings': siblings, 'project': project, 'setup': setup, 'title': title,
    }


# ---------------------------------------------------------------- summary

def _get(mapping, *keys):
    for key in keys:
        if not isinstance(mapping, dict):
            return None
        mapping = mapping.get(key)
    return mapping


def _incomplete_host(exit_info):
    return bool(exit_info.get('timed_out')) or exit_info.get('stop_reason') in HOST_LIMITS


def _status_verdict(receipt, exit_info, matches):
    status = receipt.get('status')
    if not receipt or status is None or _incomplete_host(exit_info):
        return 'incomplete'
    if status == 'pass':
        return 'diagnostic' if matches is False else 'pass'
    if status in ('fail', 'incomplete', 'diagnostic'):
        return status
    if status == 'prefix_pass':
        return 'diagnostic'
    return 'fail'


def build_summary(run):
    """Restate the receipts as one flat dict. Never raises on missing optional fields."""
    manifest, exit_info, setup = run['manifest'], run['exit'], run['setup'] or {}
    receipt = run['receipt'] if isinstance(run['receipt'], dict) else {}
    title = run['title']
    streaming = receipt.get('streaming') if isinstance(receipt.get('streaming'), dict) else {}
    binary = receipt.get('binary_sha256') or receipt.get('candidate_sha256') or _get(manifest, 'inputs', 'exe', 'sha256')
    setup_sha = receipt.get('setup_executable_sha256') or setup.get('executable_sha256')
    matches = receipt.get('binary_matches_setup')
    match_source = 'receipt' if matches is not None else None
    if matches is None and isinstance(binary, str) and isinstance(setup_sha, str):
        # Derived only: the project's setup.json may not be the receipt this
        # run was launched from, so this never downgrades the verdict.
        matches, match_source = binary.lower() == setup_sha.lower(), 'setup.json'
    receipt_matches = matches if match_source == 'receipt' else None
    summary = {
        'schema': 'psx-tas-archive-summary-v1',
        'title': TITLES.get(title, {}).get('name', 'Unknown title'), 'title_key': title,
        'run_name': run['run_dir'].name, 'run_dir': str(run['run_dir']),
        'receipt': run['receipt_name'],
        'binary_sha256': binary, 'setup_executable_sha256': setup_sha, 'binary_matches_setup': matches,
        'binary_match_source': match_source, 'diagnostic_binary': receipt.get('diagnostic_binary'),
        'source_head': setup.get('source_head'), 'source_tree': setup.get('source_tree'),
        'setup_schema': setup.get('schema'),
        'project': run['project'].name if run['project'] is not None else None,
        'setup_path': str(run['project'] / 'setup.json') if run['setup'] is not None else None,
        'route_frames': _get(manifest, 'route', 'frames'), 'route_sha256': _get(manifest, 'route', 'sha256'),
        'exit_code': exit_info.get('exit_code'), 'stop_reason': exit_info.get('stop_reason'),
        'timed_out': exit_info.get('timed_out'), 'host_seconds': exit_info.get('host_seconds'),
        'end_frame': _get(run['complete'], 'frame'),
        'prefix_input': next((p.name for s, p in run['siblings'].items() if s != '-ladder.json'), None),
        'ladder': None, 'receipt_status': receipt.get('status'),
        'streaming': ({k: streaming.get(k) for k in ('compared_returns', 'first_divergence', 'stopped_on_divergence')}
                      if streaming else None),
    }
    if run['ladder']:
        ladder = run['ladder']
        results = ladder.get('results') or []
        summary['ladder'] = {'status': ladder.get('status'), 'tiers': ladder.get('tiers'), 'endpoint': ladder.get('endpoint'),
                             'completed_tiers': [r.get('returns') for r in results if r.get('mechanical_match')],
                             'path': str(run['siblings']['-ladder.json'])}
    compared = expected = None
    scope = 'full route'
    verdict = 'incomplete'
    terminal_ram = terminal_card = None
    divergence = streaming.get('first_divergence') if streaming else None
    if title in ('biohazard', 'megamanx5'):
        comparison = receipt.get('comparison') if isinstance(receipt.get('comparison'), dict) else None
        returns = comparison.get('returns') if comparison else None
        if isinstance(returns, list) and len(returns) == 2:
            compared, expected = returns[1], returns[0]
        expected = receipt.get('observed_returns', expected)
        full = receipt.get('full_original_input_and_tail')
        prefix = receipt.get('diagnostic_prefix') is True or full is False
        scope = 'prefix' if prefix else 'full route'
        terminal_ram, terminal_card = receipt.get('terminal_ram_match'), receipt.get('terminal_card1_match')
        if divergence is None:
            divergence = receipt.get('first_divergence') or (comparison or {}).get('first_divergence')
        summary['original_inputs'] = _get(run['complete'], 'input_frames') or _get(manifest, 'route', 'frames')
        if not receipt or comparison is None or _incomplete_host(exit_info):
            verdict = 'incomplete'
        elif not comparison.get('match') or receipt.get('native_input_exit') not in (0, None):
            verdict = 'fail'
        elif not prefix and not (terminal_ram and terminal_card):
            verdict = 'fail'
        elif receipt_matches is False or prefix:
            verdict = 'diagnostic'
        else:
            verdict = 'pass'
    elif title == 'tekken3':
        compared, expected = receipt.get('compared_returns'), TEKKEN_RETURNS
        summary['original_inputs'] = receipt.get('original_inputs')
        if divergence is None:
            divergence = receipt.get('first_divergence')
        verdict = _status_verdict(receipt, exit_info, receipt_matches)
    elif title == 'pepsiman':
        captured = receipt.get('captured_returns')
        if isinstance(captured, list) and len(captured) == 2:
            compared, expected = captured[1], captured[0]
        else:
            compared = receipt.get('compared_returns')
        expected = receipt.get('source_observation_end', expected)
        terminal_ram = receipt.get('terminal_ram_matches')
        summary['original_inputs'] = receipt.get('original_inputs')
        summary['input_identity_matches'] = receipt.get('input_identity_matches')
        if divergence is None:
            divergence = receipt.get('first_divergence')
        verdict = _status_verdict(receipt, exit_info, receipt_matches)
    else:
        verdict = _status_verdict(receipt, exit_info, receipt_matches) if receipt else 'incomplete'
    summary.update({
        'scope': scope, 'returns_compared': compared, 'returns_expected': expected,
        'returns_match': (compared == expected) if compared is not None and expected is not None else None,
        'terminal_ram_match': terminal_ram, 'terminal_card1_match': terminal_card,
        'first_divergence': divergence,
        'stopped_on_divergence': streaming.get('stopped_on_divergence') if streaming else None,
        'verdict': verdict,
    })
    return summary


# --------------------------------------------------------- SHA256SUMS merge

def parse_sums(text):
    """Return ([(hash, path)...], newline, bom) from SHA256SUMS text."""
    bom = text.startswith('\ufeff')
    if bom:
        text = text[1:]
    newline = '\r\n' if '\r\n' in text else '\n'
    entries = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        digest, sep, path = line.partition('  ')
        if not sep:
            digest, sep, path = line.partition(' *')
        if not sep or len(digest) != 64:
            raise ArchiveError(f'unparseable {SUMS_NAME} line: {raw!r}')
        entries.append((digest.lower(), path.strip()))
    return entries, newline, bom


def read_sums(path):
    """SHA256SUMS text with its line endings intact (no newline translation)."""
    path = Path(path)
    if not path.exists():
        return ''
    with path.open('r', encoding='utf-8', newline='') as stream:
        return stream.read()


def merge_sums(existing_text, new_entries):
    """Append (path, sha256) pairs to SHA256SUMS text.

    Existing lines and their order are preserved. A path already listed with
    the same hash is skipped; a path listed with a different hash raises
    ArchiveError. The appended block is sorted by path. Returns
    (text, appended_lines)."""
    entries, newline, bom = parse_sums(existing_text or '')
    if not existing_text:
        newline, bom = '\n', False
    known = {}
    for digest, path in entries:
        known.setdefault(path, digest)
    appended = []
    for path, digest in sorted(set((p, d.lower()) for p, d in new_entries)):
        if path in known:
            if known[path] != digest:
                raise ArchiveError(f'{SUMS_NAME} already lists {path} as {known[path]}, refusing to record {digest}')
            continue
        appended.append(f'{digest}  {path}')
        known[path] = digest
    lines = [f'{d}  {p}' for d, p in entries] + appended
    text = ('\ufeff' if bom else '') + newline.join(lines) + (newline if lines else '')
    return text, appended


# ------------------------------------------------------------------ copying

def hash_tree(root):
    return {rel: sha256_file(path) for rel, path in walk_files(root)}


def tree_bytes(root):
    return sum(path.stat().st_size for _rel, path in walk_files(root))


def copy_run_tree(source, destination, source_hashes=None):
    """copytree source->destination. Returns (entries, bytes, already_archived).

    entries = [(relative posix path below destination, sha256)] of the files in
    the destination. When the destination exists it must hash identically to
    the source (already archived); anything else is a verification failure."""
    source, destination = Path(source), Path(destination)
    if source_hashes is None:
        source_hashes = hash_tree(source)
    if destination.exists():
        if hash_tree(destination) == source_hashes:
            return sorted(source_hashes.items()), tree_bytes(destination), True
        raise ArchiveError(f'destination exists and differs from the source: {destination}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination)
    copied = hash_tree(destination)
    if copied != source_hashes:
        bad = sorted(set(source_hashes.items()) ^ set(copied.items()))
        raise ArchiveError(f'copy verification failed for {destination}: {bad[:5]}')
    src_bytes, dst_bytes = tree_bytes(source), tree_bytes(destination)
    if src_bytes != dst_bytes:
        raise ArchiveError(f'byte totals differ: source {src_bytes} destination {dst_bytes}')
    return sorted(copied.items()), dst_bytes, False


def copy_file_verified(source, destination):
    """Copy one file; identical existing copies are accepted, others refused."""
    source, destination = Path(source), Path(destination)
    digest = sha256_file(source)
    if destination.exists():
        if sha256_file(destination) == digest:
            return digest, True
        raise ArchiveError(f'destination exists and differs from the source: {destination}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    if sha256_file(destination) != digest or destination.stat().st_size != source.stat().st_size:
        raise ArchiveError(f'copy verification failed for {destination}')
    return digest, False


def plan_paths(run, archive_dir):
    """Every destination this archive step would touch."""
    archive_dir = Path(archive_dir)
    run_dir = run['run_dir']
    plan = {'archive_dir': archive_dir, 'run_dest': archive_dir / 'runs' / run_dir.name,
            'siblings': {p: archive_dir / 'runs' / p.name for p in run['siblings'].values()},
            'builds': {}, 'sums': archive_dir / SUMS_NAME,
            'archive_json': archive_dir / 'runs' / f'{run_dir.name}.archive.json',
            'notes_dir': archive_dir / 'notes'}
    if run['setup'] is not None and run['project'] is not None:
        for name in BUILD_FILES:
            source = run['project'] / name
            if source.is_file():
                plan['builds'][source] = archive_dir / 'builds' / run['project'].name / name
    return plan


def archive(run, archive_dir):
    """Copy the run, its siblings and the build receipts; merge SHA256SUMS.

    Returns a dict describing what was copied. Raises ArchiveError on any
    verification failure. The SHA256SUMS merge is checked against the source
    hashes before anything is copied, so a conflicting hash leaves the
    archive untouched; the file is written only after every copy verified."""
    plan = plan_paths(run, archive_dir)
    archive_dir = plan['archive_dir']
    source_hashes = hash_tree(run['run_dir'])
    prefix = plan['run_dest'].relative_to(archive_dir).as_posix()
    sums_entries = [(f'{prefix}/{rel}', digest) for rel, digest in sorted(source_hashes.items())]
    extras = list(plan['siblings'].items()) + list(plan['builds'].items())
    sums_entries += [(dest.relative_to(archive_dir).as_posix(), sha256_file(source)) for source, dest in extras]
    sums_path = plan['sums']
    existing = read_sums(sums_path)
    text, appended = merge_sums(existing, sums_entries)
    entries, run_bytes, already = copy_run_tree(run['run_dir'], plan['run_dest'], source_hashes)
    extra_bytes = 0
    copied_files = []
    for source, dest in extras:
        digest, existed = copy_file_verified(source, dest)
        extra_bytes += dest.stat().st_size
        copied_files.append({'source': str(source), 'destination': str(dest), 'sha256': digest, 'already_present': existed})
    if appended:
        sums_path.parent.mkdir(parents=True, exist_ok=True)
        sums_path.write_text(text, encoding='utf-8', newline='')
    return {'already_archived': already, 'run_dest': plan['run_dest'], 'run_bytes': run_bytes,
            'source_bytes': tree_bytes(run['run_dir']), 'extra_bytes': extra_bytes,
            'file_count': len(entries), 'sums_appended': appended, 'extra_files': copied_files,
            'archive_json': plan['archive_json'], 'notes_dir': plan['notes_dir']}


# -------------------------------------------------------------- rendering

def note_stem(summary, author, when):
    return f'{when:%Y%m%d-%H%M}-{author.upper()}-{summary["title_key"]}-{summary["run_name"]}'


def describe_returns(summary):
    compared, expected = summary.get('returns_compared'), summary.get('returns_expected')
    if compared is None and expected is None:
        return 'returns not compared'
    text = f'{fmt_int(compared)}/{fmt_int(expected)} returns'
    if summary.get('scope') == 'prefix':
        text += ' (diagnostic prefix)'
    return text


def describe_terminal(summary):
    ram, card = summary.get('terminal_ram_match'), summary.get('terminal_card1_match')
    if ram is None and card is None:
        return 'terminal equality not observed'
    parts = []
    if ram is not None:
        parts.append('terminal RAM ' + ('equal' if ram else 'DIFFERS'))
    if card is not None:
        parts.append('card1 ' + ('equal' if card else 'DIFFERS'))
    return ', '.join(parts)


def describe_divergence(summary):
    divergence = summary.get('first_divergence')
    if not divergence:
        return 'none'
    if isinstance(divergence, dict):
        frame = divergence.get('frame', divergence.get('return'))
        cycles = divergence.get('source_cycle'), divergence.get('native_cycle')
        text = f'first divergence at return {fmt_int(frame)}' if frame is not None else 'first divergence recorded'
        if all(isinstance(c, int) for c in cycles):
            text += f' (cycle {cycles[0]} vs {cycles[1]})'
        if summary.get('stopped_on_divergence'):
            text += ', run stopped there'
        return text
    return f'first divergence at {divergence}'


def describe_binary_match(summary):
    """Suffix describing how the binary relates to the setup receipt, or ''."""
    matches, origin = summary.get('binary_matches_setup'), summary.get('binary_match_source')
    setup_sha = short(summary.get('setup_executable_sha256'))
    if matches is True:
        return ' (matches the setup receipt)' if origin == 'receipt' else f' (matches {summary.get("project")}/setup.json)'
    if matches is False:
        if origin == 'receipt':
            return f' (DIAGNOSTIC binary declared in the receipt; setup receipt executable is {setup_sha})'
        return f' (differs from {summary.get("project")}/setup.json executable {setup_sha}; receipt carries no identity flag)'
    return ''


def describe_binary(summary):
    return (summary.get('binary_sha256') or 'unknown') + describe_binary_match(summary)


def describe_tree(summary):
    head, tree = summary.get('source_head'), summary.get('source_tree')
    if not head and not tree:
        return 'an unrecorded source tree (setup receipt not reachable)'
    return f'source tree {short(tree)} (head {short(head)})'


def render_note(summary, evidence_path, options, when):
    """The mailbox note: header block then a short results section."""
    receipt = summary.get('receipt')
    receipt_path = f'{evidence_path}{os.sep}{receipt}' if receipt else 'no title receipt'
    label = f'{options["label"]}: ' if options.get('label') else ''
    subject = (f'{label}{summary["title"]} {summary["run_name"]}: {summary["verdict"].upper()} '
               f'({describe_returns(summary)})')
    lines = [f'From: {options["author"]} (workbench)', f'To: {options["to"]}', f'CC: {options["cc"]}',
             f'Date: {when:%Y-%m-%d %H:%M} (local)', f'Task: {options["task"]}', f'Subject: {subject}', '',
             f'Results ({summary["scope"]}, verdict per the receipt: {summary["verdict"]}):',
             f'- Title: {summary["title"]}, run {summary["run_name"]}',
             f'- Tree: {describe_tree(summary)}', f'- Binary: {describe_binary(summary)}',
             f'- Returns: {describe_returns(summary)}', f'- Terminal: {describe_terminal(summary)}',
             f'- Divergence: {describe_divergence(summary)}']
    if summary.get('ladder'):
        ladder = summary['ladder']
        lines.append(f'- Ladder: {ladder.get("status")} over tiers {ladder.get("tiers")} '
                     f'(completed {ladder.get("completed_tiers")}, endpoint {fmt_int(ladder.get("endpoint"))})')
    if summary.get('prefix_input'):
        lines.append(f'- Prefix route: {summary["prefix_input"]}')
    if isinstance(summary.get('host_seconds'), (int, float)):
        lines.append(f'- Host: {summary["host_seconds"]:.0f} s, exit {summary.get("exit_code")}, '
                     f'stop reason {summary.get("stop_reason") or "none"}')
    lines += [f'- Evidence: {evidence_path}', f'- Receipt: {receipt_path}', '',
              'Archived by archive_run.py; this note restates the receipt and qualifies nothing.', '']
    return '\n'.join(lines)


def render_board(summary, evidence_path, options):
    """One plain-prose paragraph for a status board."""
    label = f'{options["label"]}: ' if options.get('label') else ''
    binary = summary.get('binary_sha256')
    binary_text = (f'binary {short(binary)}' if binary else 'an unrecorded binary') + describe_binary_match(summary)
    what = 'a diagnostic prefix replay' if summary.get('scope') == 'prefix' else 'a full-route replay'
    sentence = (f'{label}{summary["title"]} run {summary["run_name"]} was {what} on {describe_tree(summary)} '
                f'with {binary_text}; it compared {describe_returns(summary).replace(" (diagnostic prefix)", "")}')
    terminal = describe_terminal(summary)
    if terminal != 'terminal equality not observed':
        sentence += f', {terminal}'
    divergence = describe_divergence(summary)
    sentence += ', no divergence' if divergence == 'none' else f', {divergence}'
    if summary.get('ladder'):
        sentence += (f'; the ladder over tiers {summary["ladder"].get("tiers")} reported '
                     f'{summary["ladder"].get("status")}')
    if isinstance(summary.get('host_seconds'), (int, float)):
        sentence += f'; host time {summary["host_seconds"]:.0f} s'
    sentence += (f'. Verdict per the receipt: {summary["verdict"]}. Evidence: {evidence_path} '
                 f'({summary.get("receipt") or "no title receipt"}; hashes in {SUMS_NAME}).')
    return sentence + '\n'


# ------------------------------------------------------------------- main

def write_text(path, text):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text, encoding='utf-8', newline='\n')


def build_archive_record(summary, result, run, author, when):
    receipts = {name: sha256_file(run['run_dir'] / name) for name in run['receipts']}
    if run['ladder'] is not None:
        receipts[run['siblings']['-ladder.json'].name] = sha256_file(run['siblings']['-ladder.json'])
    return {'schema': 'psx-tas-archive-v1', 'archived_at': when.isoformat(timespec='seconds'), 'author': author,
            'already_archived': result['already_archived'], 'run_dest': str(result['run_dest']),
            'file_count': result['file_count'], 'run_bytes': result['run_bytes'], 'source_bytes': result['source_bytes'],
            'bytes_match': result['run_bytes'] == result['source_bytes'], 'extra_bytes': result['extra_bytes'],
            'extra_files': result['extra_files'], 'sums_appended': len(result['sums_appended']),
            'receipt_sha256': receipts, 'summary': summary}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run_dir', type=Path)
    parser.add_argument('archive_name')
    parser.add_argument('--evidence-root', type=Path,
                        default=Path(os.environ.get('PSX_TAS_EVIDENCE') or DEFAULT_EVIDENCE_ROOT))
    parser.add_argument('--author', default='pegasus-claude')
    parser.add_argument('--to', default='eagle-codex, eagle-claude')
    parser.add_argument('--cc', default='operator')
    parser.add_argument('--task', default='TAS replay lane - evidence archive')
    parser.add_argument('--mailbox', type=Path, help='also drop the note and its .md.sha256 sidecar here')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--label', help='free-text label prefixed to the subject and board paragraph')
    args = parser.parse_args(argv)
    if not args.archive_name or any(c in args.archive_name for c in '/\\'):
        parser.error('archive name must be a single path component')
    try:
        run = load_run(args.run_dir)
    except UsageError as error:
        parser.error(str(error))
    archive_dir = args.evidence_root / args.archive_name
    options = {'author': args.author, 'to': args.to, 'cc': args.cc, 'task': args.task, 'label': args.label}
    summary = build_summary(run)
    plan = plan_paths(run, archive_dir)
    evidence_path = str(plan['run_dest'])
    when = local_now()
    existing_record = load_json(plan['archive_json'])
    if existing_record and existing_record.get('archived_at'):
        # Re-runs keep the first archive timestamp so note names stay stable.
        try:
            when = datetime.datetime.fromisoformat(existing_record['archived_at'])
        except ValueError:
            pass
    stem = note_stem(summary, args.author, when)
    note = render_note(summary, evidence_path, options, when)
    board = render_board(summary, evidence_path, options)
    if args.dry_run:
        print('DRY RUN: nothing copied or written')
        print(json.dumps(summary, indent=2))
        print('would copy run to:', plan['run_dest'])
        for source, dest in plan['siblings'].items():
            print('would copy sibling:', source, '->', dest)
        for source, dest in plan['builds'].items():
            print('would copy build receipt:', source, '->', dest)
        print('would update:', plan['sums'])
        print('would write:', plan['archive_json'])
        print('would write note:', plan['notes_dir'] / f'{stem}.md')
        print('would write board:', plan['notes_dir'] / f'{stem}-board.md')
        if args.mailbox:
            print('would drop note into mailbox:', args.mailbox / f'{stem}.md', '(+ .md.sha256)')
        print('--- note ---')
        print(note, end='')
        print('--- board ---')
        print(board, end='')
        return 0
    try:
        result = archive(run, archive_dir)
        record = build_archive_record(summary, result, run, args.author, when)
        write_text(result['archive_json'], json.dumps(record, indent=2) + '\n')
        write_text(result['notes_dir'] / f'{stem}.md', note)
        write_text(result['notes_dir'] / f'{stem}-board.md', board)
        if args.mailbox:
            write_text(args.mailbox / f'{stem}.md', note)
            write_text(args.mailbox / f'{stem}.md.sha256', f'{sha256_text(note)}  {stem}.md\n')
    except ArchiveError as error:
        print(f'archive_run: verification failure: {error}', file=sys.stderr)
        return 1
    state = 'already archived' if result['already_archived'] else 'archived'
    print(f'{state}: {result["run_dest"]} ({result["file_count"]} files, {result["run_bytes"]} bytes, '
          f'{len(result["sums_appended"])} SHA256SUMS lines appended)')
    print(f'archive record: {result["archive_json"]}')
    print(f'note: {result["notes_dir"] / (stem + ".md")}')
    if args.mailbox:
        print(f'mailbox: {args.mailbox / (stem + ".md")}')
    print(board, end='')
    return 0


if __name__ == '__main__':
    sys.exit(main())
