#!/usr/bin/env python3
"""Content-keyed build cache for the TAS title setups (docs/tasreplays/build-cache.md).

Scope: build REUSE keyed by content identity — git tree/blob ids of the committed
inputs, SHA-256 of every file input, the toolchain banner lines and the CMake
flags that shape the output. It is never a verification shortcut: media, movie,
reference and firmware hashes, the BIOS emitter fingerprint and the Tekken
codegen guard run on every setup whether or not a stage was reused, and `run`
does not consult the cache at all. A reused stage is always recorded in the
setup receipt (`build_cache` block) with the entry it came from, when that entry
was built and by which source head.

Three stages, each keyed by a canonical JSON document (sorted keys, no
whitespace) hashed with SHA-256:

  tools     — psxrecomp-bios/game/toml.exe + ctest, keyed by the committed trees the
              tools build compiles, the CMake flags (minus -DPython3_EXECUTABLE) and the
              toolchain identity.
  generated — BIOS C + game C, keyed by the tool executables, the BIOS inputs (ROM, seeds,
              profile template, emitter fingerprint) and the game inputs (boot executable,
              path-normalized game.toml, seeds or census).
  native    — the player executable, keyed by every generated file, the runtime trees,
              tools/tasreplays/CMakeLists.txt, the CMake flags (minus the three path-only
              flags), the path-normalized BIOS profile and the toolchain identity.

Paths, mtimes and branch names are never key inputs. A partial, corrupt or
mismatching entry is a miss, never an error. Entries are written to
`<entry>.partial-<pid>` and renamed into place; a rename that loses to a
concurrent writer discards its own copy and uses the (re-verified) winner.
Nothing in setup deletes; `list` and `prune --keep-days N` are the operator's tools.

Standard library only.
"""
from __future__ import annotations

import argparse
import datetime
import functools
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SCHEMA = 'psx-tas-build-cache-v1'
STAGES = ('tools', 'generated', 'native')
ENV_VAR = 'PSX_TAS_BUILD_CACHE'

# Stage 1 compiles recompiler/ and, with BUILD_TESTING=ON, tools/tasreplays/tests
# (runtime/tests/*.c, runtime/src/psx_sha256.c and friends) and runs the python tests.
TOOLS_TREES = ('recompiler', 'runtime', 'tools/tasreplays', 'lib', 'cmake', 'third_party')
# host/ is compiled into the player, bios/ carries the bundled OpenBIOS image and the
# profile templates, lib/ holds the optional netplay/rewind sources the flags may enable.
NATIVE_TREES = ('runtime', 'cmake', 'third_party', 'assets', 'mods', 'host', 'bios', 'lib')
TAS_CMAKE = 'tools/tasreplays/CMakeLists.txt'
# -DPython3_EXECUTABLE only selects the interpreter that runs the tests.
TOOLS_DROP = ('-DPython3_EXECUTABLE=',)
# -DTAS_PROJECT_DIR only locates files already keyed by content; the BIOS profile
# is read at configure time for the staleness warning only; bash is a host detail.
NATIVE_DROP = ('-DTAS_PROJECT_DIR=', '-DPSXRECOMP_BIOS_PROFILE=', '-D_psxrt_bash=')
TOOL_EXES = ('psxrecomp-bios.exe', 'psxrecomp-game.exe', 'psxrecomp-toml.exe')
TOOLS_LOGS = ('configure-tools.log', 'build-tools.log', 'test-tools.log')
GENERATED_LOGS = ('generate-bios.log', 'generate-game.log')
NATIVE_LOGS = ('configure-native.log', 'build-native.log')
BIOS_GENERATED = ('{stem}_full.c', '{stem}_dispatch.c', '{stem}_skipped_functions.json')
# TOML keys whose values are absolute paths in the project's game.toml/bios.toml, and
# what replaces each value in the normalized form: 'file' = SHA-256 of the file named,
# 'toml' = the normalized SHA-256 of the TOML named (bios_config -> bios.toml, which
# itself embeds paths), anything else = that literal (out_dir is an output location).
PATH_KEYS = {'exe': 'file', 'seeds': 'file', 'bios_config': 'toml', 'out_dir': 'out_dir', 'rom': 'file'}
HEX64 = re.compile('[0-9a-f]{64}')


# ---------------------------------------------------------------- identities

def digest(path) -> str:
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def key_of(document) -> str:
    """SHA-256 of the canonical JSON form (sorted keys, no whitespace)."""
    return hashlib.sha256(json.dumps(document, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')


def _git(repo, *args) -> str:
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()


def tree_ids(repo, dirs) -> dict:
    """Git tree object ids of committed directories (content hashes; setup requires a clean tree)."""
    dirs = list(dirs)
    lines = _git(repo, 'rev-parse', *[f'HEAD:{d}' for d in dirs]).splitlines()
    if len(lines) != len(dirs):
        raise ValueError(f'git rev-parse returned {len(lines)} ids for {len(dirs)} trees')
    return dict(zip(dirs, lines))


def blob_id(repo, path) -> str:
    """Git blob id of a committed file, given relative to the repo or absolute inside it."""
    relative = Path(path)
    if relative.is_absolute():
        relative = relative.resolve().relative_to(Path(repo).resolve())
    return _git(repo, 'rev-parse', f'HEAD:{relative.as_posix()}')


def _first_line(argv) -> str:
    return subprocess.check_output(argv, text=True).splitlines()[0].strip()


@functools.lru_cache(maxsize=None)
def _toolchain_identity() -> tuple:
    return tuple({
        'gcc': _first_line(['gcc', '--version']),
        'gcc_machine': _first_line(['gcc', '-dumpmachine']),
        'g++': _first_line(['g++', '--version']),
        'cmake': _first_line(['cmake', '--version']),
        'ninja': _first_line(['ninja', '--version']),
        'python': '%d.%d.%d' % sys.version_info[:3],
    }.items())


def toolchain_identity() -> dict:
    """Banner lines of gcc/g++/cmake/ninja, the gcc target and the python version."""
    return dict(_toolchain_identity())


def cmake_key_args(argv, drop=()) -> list:
    """The -D flags (and the -G generator) of a cmake configure argv, sorted, minus `drop` prefixes.

    -S/-B and their values are locations, not inputs; the program name and bare values are ignored.
    """
    kept = []
    tokens = iter(str(token) for token in argv)
    for token in tokens:
        if token in ('-S', '-B'):
            next(tokens, None)
            continue
        if token == '-G':
            kept.append('-G' + next(tokens))
            continue
        if token.startswith('-D') and not any(token.startswith(prefix) for prefix in drop):
            kept.append(token)
    return sorted(kept)


# ---------------------------------------------------------- TOML normalization

_TOML_LINE = re.compile(r'^(\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*=\s*)"([^"]*)"(.*)$')


def _named(base: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else base / path


def normalized_toml_text(path, path_keys=PATH_KEYS, base=None) -> str:
    """The TOML with every path value replaced by the identity of what it names.

    Line endings are canonicalized to LF. Relative path values resolve against `base` (default:
    the TOML's own directory; the setups always write absolute values). A nested TOML ('toml'
    mode) is normalized with the default PATH_KEYS so a caller's literal override (e.g.
    seeds -> 'census' for the game TOML) never leaks into the BIOS profile it names.
    """
    toml = Path(path)
    base = Path(base) if base is not None else toml.parent
    text = toml.read_bytes().decode('utf8').replace('\r\n', '\n')
    lines = []
    for line in text.split('\n'):
        match = _TOML_LINE.match(line)
        if match and match[2] in path_keys:
            mode = path_keys[match[2]]
            if mode == 'file':
                replacement = digest(_named(base, match[4]))
            elif mode == 'toml':
                replacement = normalized_toml_sha(_named(base, match[4]), PATH_KEYS, base)
            else:
                replacement = mode
            line = f'{match[1]}{match[2]}{match[3]}"{replacement}"{match[5]}'
        lines.append(line)
    return '\n'.join(lines)


def normalized_toml_sha(path, path_keys=PATH_KEYS, base=None) -> str:
    return hashlib.sha256(normalized_toml_text(path, path_keys, base).encode()).hexdigest()


def portable_emitter_fingerprint(bash, repo, profile) -> str:
    """tools/bios_emitter_fingerprint.sh over the path-normalized copy of a BIOS profile.

    The script hashes the profile bytes themselves, and the project's bios.toml embeds absolute
    paths, so the stamp written to generated/<stem>.emitter.sha differs between two projects with
    identical content. For the key the script runs on the normalized text instead: it then covers
    the emitter sources and cycle-model headers plus the profile's content; the ROM and seeds it
    would otherwise read through the profile's paths are keyed separately by SHA-256.
    """
    with tempfile.TemporaryDirectory() as directory:
        copy = Path(directory) / 'profile.toml'
        copy.write_text(normalized_toml_text(profile), encoding='utf8', newline='\n')
        script = (Path(repo) / 'tools/bios_emitter_fingerprint.sh').as_posix()
        output = subprocess.check_output([str(bash), script, copy.as_posix()], cwd=str(repo), text=True).strip()
    if not HEX64.fullmatch(output):
        raise ValueError('invalid portable BIOS emitter fingerprint')
    return output


# ------------------------------------------------------------ key documents

def tools_inputs(repo, cmake_argv, toolchain=None) -> dict:
    return {'trees': tree_ids(repo, TOOLS_TREES),
            'cmake_args': cmake_key_args(cmake_argv, TOOLS_DROP),
            'toolchain': toolchain or toolchain_identity()}


def generated_inputs(tools_dir, stem, rom, bios_seeds, profile_template_blob, emitter_fingerprint,
                     boot_exe, game_toml, game_seeds=None) -> dict:
    """game_seeds is the seeds file psxrecomp-game reads, or None when the census produces it."""
    tools_dir = Path(tools_dir)
    path_keys = PATH_KEYS if game_seeds is not None else {**PATH_KEYS, 'seeds': 'census'}
    return {'tools': {'bios_exe_sha': digest(tools_dir / 'psxrecomp-bios.exe'),
                      'game_exe_sha': digest(tools_dir / 'psxrecomp-game.exe'),
                      'toml_exe_sha': digest(tools_dir / 'psxrecomp-toml.exe')},
            'bios': {'stem': stem, 'rom_sha': digest(rom), 'seeds_sha': digest(bios_seeds),
                     'profile_template_blob': profile_template_blob,
                     'emitter_fingerprint': emitter_fingerprint},
            'game': {'boot_exe_sha': digest(boot_exe),
                     'game_toml_normalized_sha': normalized_toml_sha(game_toml, path_keys),
                     'seeds': 'census' if game_seeds is None else digest(game_seeds)}}


def generated_set(repo, stem, project) -> dict:
    """Entry-relative name -> path for the BIOS-stem files and every <project>/generated file."""
    files = {}
    for pattern in BIOS_GENERATED:
        name = pattern.format(stem=stem)
        path = Path(repo) / 'generated' / name
        if not path.is_file():
            raise ValueError(f'missing generated BIOS file: {path}')
        files[f'bios/{name}'] = path
    game = Path(project) / 'generated'
    if not game.is_dir():
        raise ValueError(f'missing generated game directory: {game}')
    for path in sorted(game.rglob('*')):
        if path.is_file():
            files['game/' + path.relative_to(game).as_posix()] = path
    return files


def native_inputs(repo, generated_files, cmake_argv, bios_profile, toolchain=None) -> dict:
    return {'generated': {name: digest(path) for name, path in sorted(generated_files.items())},
            'trees': tree_ids(repo, NATIVE_TREES),
            'tas_cmake': blob_id(repo, TAS_CMAKE),
            'cmake_args': cmake_key_args(cmake_argv, NATIVE_DROP),
            'bios_profile_normalized_sha': normalized_toml_sha(bios_profile),
            'toolchain': toolchain or toolchain_identity()}


def tools_key(repo, cmake_argv, toolchain=None) -> str:
    return key_of(tools_inputs(repo, cmake_argv, toolchain))


def generated_key(*args, **kwargs) -> str:
    return key_of(generated_inputs(*args, **kwargs))


def native_key(*args, **kwargs) -> str:
    return key_of(native_inputs(*args, **kwargs))


# ----------------------------------------------------------------- entries

class Entry:
    def __init__(self, path: Path, receipt: dict):
        self.path = Path(path)
        self.receipt = receipt

    def __repr__(self):
        return f'Entry({self.path})'


def entry_dir(root, stage, key) -> Path:
    if stage not in STAGES or not HEX64.fullmatch(key):
        raise ValueError(f'invalid cache stage/key: {stage} {key}')
    return Path(root) / stage / key


def new_partial(root, stage, key) -> Path:
    """A fresh `<entry>.partial-<pid>` directory; never read by lookup."""
    partial = entry_dir(root, stage, key).with_name(f'{key}.partial-{os.getpid()}')
    if partial.exists():
        shutil.rmtree(partial)
    partial.mkdir(parents=True)
    return partial


def _listed(receipt: dict) -> dict:
    listed = dict(receipt['files'])
    listed.update({f'logs/{name}': sha for name, sha in receipt.get('logs', {}).items()})
    return listed


def verify(entry_path, stage, key) -> Entry | None:
    """The entry when its receipt is well-formed and every listed file hashes as recorded."""
    entry_path = Path(entry_path)
    try:
        receipt = json.loads((entry_path / 'receipt.json').read_text(encoding='utf8'))
        if receipt.get('schema') != SCHEMA or receipt.get('stage') != stage or receipt.get('key') != key:
            return None
        if not receipt['files']:
            return None  # an entry without output files is not an entry
        listed = _listed(receipt)
        for name, sha in listed.items():
            if not HEX64.fullmatch(str(sha)) or digest(entry_path / name) != sha:
                return None
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return None
    return Entry(entry_path, receipt)


def lookup(root, stage, key) -> Entry | None:
    """A verified entry for the key, or None (partial, corrupt or missing entries are misses)."""
    if root is None:
        return None
    return verify(entry_dir(root, stage, key), stage, key)


def store(root, stage, key, files, logs, receipt, partial=None) -> Entry:
    """Copy `files` (entry name -> source) and `logs` (name -> source) into a partial entry,
    write its receipt and rename it into place. A source already inside the partial entry
    (stage 1 builds there) is hashed in place. Returns the entry to use: this one, or the
    verified winner of a concurrent store of the same key.
    """
    root = Path(root)
    partial = Path(partial) if partial else new_partial(root, stage, key)
    receipt = {'schema': SCHEMA, 'stage': stage, 'key': key, **receipt, 'files': {}, 'logs': {}}
    for name, source in files.items():
        target = partial / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if not (target.exists() and target.samefile(source)):
            shutil.copyfile(source, target)
        receipt['files'][name] = digest(target)
    for name, source in logs.items():
        target = partial / 'logs' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        receipt['logs'][name] = digest(target)
    (partial / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf8', newline='\n')
    return _commit(root, stage, key, partial, receipt)


def _commit(root, stage, key, partial, receipt) -> Entry:
    final = entry_dir(root, stage, key)
    if not final.exists():
        try:
            os.rename(partial, final)
            return Entry(final, receipt)
        except OSError:
            if not final.exists():
                raise
    # Lost the race: a complete entry for this key exists. Use it if it verifies; a
    # complete-but-corrupt entry is replaced (it would only ever be a miss).
    winner = verify(final, stage, key)
    if winner is not None:
        shutil.rmtree(partial, ignore_errors=True)
        return winner
    shutil.rmtree(final)
    os.rename(partial, final)
    return Entry(final, receipt)


def copy_in(entry: Entry, targets: dict) -> None:
    """Copy entry files (entry name -> destination) and re-hash each destination against the receipt."""
    for name, destination in targets.items():
        expected = entry.receipt['files'][name]
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(entry.path / name, destination)
        if digest(destination) != expected:
            raise RuntimeError(f'build cache copy of {name} from {entry.path} does not hash as its receipt')


def provenance_line(entry: Entry) -> str:
    return (f'# build-cache hit: this stage was reused from {entry.path} '
            f'(built {entry.receipt.get("built_at")}, source_head {entry.receipt.get("source_head")}); '
            f'the log below is from that build\n')


def copy_logs(entry: Entry, project) -> list:
    """Copy the entry's stage logs into the project, each prefixed by a line naming the entry."""
    written = []
    for name in entry.receipt.get('logs', {}):
        target = Path(project) / name
        target.write_bytes(provenance_line(entry).encode() + (entry.path / 'logs' / name).read_bytes())
        written.append(target)
    return written


# ----------------------------------------------------------------- receipts

def stage_record(key, hit, entry: Entry | None, built_at=None, source_head=None) -> dict:
    if entry is not None:
        built_at, source_head = entry.receipt.get('built_at'), entry.receipt.get('source_head')
    return {'key': key, 'hit': bool(hit), 'entry': str(entry.path) if entry else None,
            'built_at': built_at, 'source_head': source_head}


def receipt_block(root, tools: dict, generated: dict, native: dict) -> dict:
    return {'root': str(root) if root else None, 'tools': tools, 'generated': generated, 'native': native}


# ------------------------------------------------------------------ stages

def _announce(stage, key, entry: Entry | None):
    if entry is None:
        print(f'Build cache miss ({stage}): key {key}', flush=True)
    else:
        print(f'Build cache hit ({stage}): {entry.path} built {entry.receipt.get("built_at")} '
              f'at source_head {entry.receipt.get("source_head")}', flush=True)


def stage_tools(root, repo, project, explicit_dir, inputs, miss, head):
    """Stage 1. `explicit_dir` (--tools-dir) bypasses the cache and rebuilds there as before.
    `inputs()` returns the key document; `miss(tools_dir)` runs configure, build and ctest.
    Returns (tools build directory, receipt record)."""
    project = Path(project)
    if explicit_dir is not None:
        miss(Path(explicit_dir))
        return Path(explicit_dir), stage_record(None, False, None, now_iso(), head)
    if root is None:
        tools_dir = project / 'tools'
        miss(tools_dir)
        return tools_dir, stage_record(None, False, None, now_iso(), head)
    document = inputs()
    key = key_of(document)
    entry = lookup(root, 'tools', key)
    _announce('tools', key, entry)
    if entry is not None:
        copy_logs(entry, project)
        return entry.path / 'build', stage_record(key, True, entry)
    partial = new_partial(root, 'tools', key)
    tools_dir = partial / 'build'
    try:
        miss(tools_dir)
    except BaseException:
        shutil.rmtree(partial, ignore_errors=True)  # a failed configure/build/ctest leaves no entry
        raise
    entry = store(root, 'tools', key, {f'build/{name}': tools_dir / name for name in TOOL_EXES},
                  {name: project / name for name in TOOLS_LOGS},
                  {'inputs': document, 'ctest_exit': 0, 'built_at': now_iso(), 'source_head': head,
                   'project': str(project)}, partial=partial)
    return entry.path / 'build', stage_record(key, False, entry)


def _generated_target(repo, project, name) -> Path:
    if name.startswith('bios/'):
        return Path(repo) / 'generated' / name[len('bios/'):]
    if name.startswith('game/'):
        return Path(project) / 'generated' / name[len('game/'):]
    return Path(project) / name


def stage_generated(root, repo, project, stem, inputs, census, miss, head) -> dict:
    """Stage 2. `miss()` runs census (when `census`), BIOS and game generation as today.
    On a hit the entry's files are copied into <repo>/generated, <project>/generated (and
    census.toml/seeds.txt) and re-hashed. The caller recomputes the emitter fingerprint and
    runs its codegen guard afterwards on both paths. Returns the receipt record."""
    project = Path(project)
    if root is None:
        miss()
        return stage_record(None, False, None, now_iso(), head)
    document = inputs()
    key = key_of(document)
    entry = lookup(root, 'generated', key)
    _announce('generated', key, entry)
    if entry is not None:
        copy_in(entry, {name: _generated_target(repo, project, name) for name in entry.receipt['files']})
        copy_logs(entry, project)
        return stage_record(key, True, entry)
    miss()
    files = generated_set(repo, stem, project)
    logs = ()
    if census:
        files.update({'census.toml': project / 'census.toml', 'seeds.txt': project / 'seeds.txt'})
        logs = ('census.log',)
    entry = store(root, 'generated', key, files, {name: project / name for name in logs + GENERATED_LOGS},
                  {'inputs': document, 'built_at': now_iso(), 'source_head': head, 'project': str(project)})
    return stage_record(key, False, entry)


def stage_native(root, repo, project, native_dir, exe_name, inputs, miss, head) -> dict:
    """Stage 3. `miss()` configures and builds into `native_dir` as today. On a hit only the
    executable is copied to <native_dir>/<exe_name>.exe (no CMake build directory) and
    re-hashed. Returns the receipt record."""
    project, native_dir = Path(project), Path(native_dir)
    executable = f'{exe_name}.exe'
    if root is None:
        miss()
        return stage_record(None, False, None, now_iso(), head)
    document = inputs()
    key = key_of(document)
    entry = lookup(root, 'native', key)
    _announce('native', key, entry)
    if entry is not None:
        copy_in(entry, {executable: native_dir / executable})
        copy_logs(entry, project)
        return stage_record(key, True, entry)
    miss()
    entry = store(root, 'native', key, {executable: native_dir / executable},
                  {name: project / name for name in NATIVE_LOGS},
                  {'inputs': document, 'built_at': now_iso(), 'source_head': head, 'project': str(project)})
    return stage_record(key, False, entry)


# ------------------------------------------------------- location and CLI

def default_root(explicit=None) -> Path:
    """--build-cache, else $PSX_TAS_BUILD_CACHE, else %LOCALAPPDATA%\\psxrecomp\\build-cache."""
    if explicit:
        return Path(explicit).resolve()
    if os.environ.get(ENV_VAR):
        return Path(os.environ[ENV_VAR]).resolve()
    local = os.environ.get('LOCALAPPDATA')
    base = Path(local) if local else Path.home() / '.cache'
    return base / 'psxrecomp' / 'build-cache'


def add_arguments(parser) -> None:
    parser.add_argument('--build-cache', type=Path, metavar='DIR',
                        help=f'build cache directory (default: ${ENV_VAR} or %%LOCALAPPDATA%%\\psxrecomp\\build-cache)')
    parser.add_argument('--no-build-cache', action='store_true', help='build every stage fresh and store nothing')


def resolve_root(args) -> Path | None:
    return None if getattr(args, 'no_build_cache', False) else default_root(getattr(args, 'build_cache', None))


def entries(root):
    """Yield (stage, path, status, receipt) for every entry and partial directory under root."""
    root = Path(root)
    for stage in STAGES:
        stage_path = root / stage
        if not stage_path.is_dir():
            continue
        for path in sorted(stage_path.iterdir()):
            if not path.is_dir():
                continue
            if '.partial-' in path.name:
                yield stage, path, 'partial', None
                continue
            entry = verify(path, stage, path.name)
            if entry is None:
                try:
                    receipt = json.loads((path / 'receipt.json').read_text(encoding='utf8'))
                except (OSError, ValueError):
                    receipt = None
                yield stage, path, 'invalid', receipt
            else:
                yield stage, path, 'ok', entry.receipt


def _built_at(receipt) -> float | None:
    try:
        return datetime.datetime.fromisoformat(receipt['built_at']).timestamp()
    except (KeyError, TypeError, ValueError):
        return None


def prune(root, keep_days: float, now: float | None = None) -> list:
    """Delete entries built more than keep_days ago and partial directories not touched since."""
    now = time.time() if now is None else now
    cutoff = now - keep_days * 86400
    removed = []
    for stage, path, status, receipt in list(entries(root)):
        if status == 'partial':
            age_stamp = path.stat().st_mtime
        else:
            age_stamp = _built_at(receipt)
            if age_stamp is None:
                age_stamp = path.stat().st_mtime
        if age_stamp < cutoff:
            shutil.rmtree(path)
            removed.append(path)
    return removed


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Inspect or prune the TAS setup build cache.')
    parser.add_argument('--build-cache', type=Path, metavar='DIR')
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('list', help='list every entry with its status, build time and source head')
    pruner = sub.add_parser('prune', help='delete entries older than --keep-days')
    pruner.add_argument('--keep-days', type=float, required=True)
    args = parser.parse_args(argv)
    root = default_root(args.build_cache)
    if args.action == 'list':
        print(f'build cache: {root}')
        count = 0
        for stage, path, status, receipt in entries(root):
            receipt = receipt or {}
            print(f'{stage:9} {status:8} {path.name}  built_at={receipt.get("built_at")}  '
                  f'source_head={receipt.get("source_head")}  project={receipt.get("project")}')
            count += 1
        if not count:
            print('(empty)')
        return 0
    if args.keep_days < 0:
        parser.error('--keep-days must be non-negative')
    for path in prune(root, args.keep_days):
        print(f'removed {path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
