"""Accept .chd disc containers without changing how media is verified.

A .chd input is extracted with chdman into a private cache directory keyed by
the container's own SHA-256, split into one .bin per track, and renamed to the
original redump track layout. The resolved .cue is then handed to the caller's
existing pinned size/hash check, which admits or rejects it exactly as it would
a .cue the operator passed directly: nothing here compares, relaxes or
substitutes an expected identity, and a container whose contents differ fails
with the same error a wrong .cue produces. A non-.chd path is returned
untouched, so .cue input keeps its current code path byte for byte.

This module also holds the shared Git for Windows bash lookup used for
tools/bios_emitter_fingerprint.sh and the -D_psxrt_bash= CMake define.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
# Track files as chdman writes them into the cue it generates for a split dump.
CUE_FILE = re.compile(rb'FILE\s+"([^"\r\n]+)"\s+BINARY', re.I)
# Searched only after --chdman, PSX_CHDMAN and PATH have all come up empty.
CHDMAN_FALLBACKS = (ROOT / 'tools/mame-0.289/chdman.exe',
                    Path('D:/psxrecomp/tools/mame-0.289/chdman.exe'))
# Git for Windows ships the MSYS bash at both of these, relative to its root.
BASH_RELATIVE = ('bin/bash.exe', 'usr/bin/bash.exe')
MSYSTEM_MARKER = re.compile(r'MINGW|MSYS|UCRT|CLANG', re.I)
BASH_BUILD_MARKER = re.compile(r'msys|mingw', re.I)
_bash = None


def digest(path: Path) -> str:
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def find_chdman(explicit=None) -> Path:
    """Locate chdman: explicit argument, PSX_CHDMAN, PATH, then known installs."""
    candidates = [Path(explicit)] if explicit else []
    if os.environ.get('PSX_CHDMAN'):
        candidates.append(Path(os.environ['PSX_CHDMAN']))
    found = shutil.which('chdman')
    if found:
        candidates.append(Path(found))
    candidates += list(CHDMAN_FALLBACKS)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise ValueError('chdman is required to read a .chd container; pass --chdman, '
                     'set PSX_CHDMAN, or put chdman on PATH')


def chd_cache(explicit=None) -> Path:
    """Parent of the per-container extraction directories."""
    if explicit:
        return Path(explicit)
    if os.environ.get('PSX_CHD_CACHE'):
        return Path(os.environ['PSX_CHD_CACHE'])
    base = os.environ.get('LOCALAPPDATA') or os.environ.get('XDG_CACHE_HOME')
    return (Path(base) if base else Path.home() / '.cache') / 'psxrecomp/chd'


def apply_redump_names(cue: Path, stem: str) -> list[Path]:
    """Name the tracks the way the original dump does, and report them in order.

    chdman always suffixes ' (Track 1)'; a redump single-track dump is plain
    '<stem>.bin', and its .cue text is pinned by name in some harnesses.
    """
    data = cue.read_bytes()
    names = CUE_FILE.findall(data)
    if not names:
        raise ValueError(f'chdman wrote a cue with no BINARY tracks: {cue}')
    if len(names) == 1 and os.fsdecode(names[0]) != stem + '.bin':
        wanted = stem + '.bin'
        (cue.parent / os.fsdecode(names[0])).rename(cue.parent / wanted)
        data = data.replace(b'"' + names[0] + b'"', b'"' + os.fsencode(wanted) + b'"')
        cue.write_bytes(data)
        names = CUE_FILE.findall(data)
    tracks = [cue.parent / os.fsdecode(name) for name in names]
    for track in tracks:
        if not track.is_file():
            raise ValueError(f'extracted cue references a missing track: {track}')
    return tracks


def extract_chd(chd: Path, *, cache=None, chdman=None, stem=None) -> Path:
    """Split a .chd into its original BIN/CUE layout and return the .cue."""
    tool = find_chdman(chdman)
    chd = Path(chd).resolve(strict=True)
    stem = stem or chd.stem
    if not stem or Path(stem).name != stem:
        raise ValueError(f'invalid extracted disc name: {stem}')
    # Key the cache on the container's own bytes: a different .chd never reuses
    # another's extraction, and the same .chd is only ever split once.
    target = chd_cache(cache).resolve() / digest(chd)
    cue = target / (stem + '.cue')
    if cue.is_file():
        return cue
    staging = target.with_name(target.name + f'.partial-{os.getpid()}')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        print(f'Splitting {chd.name} into its original tracks under {target}', flush=True)
        result = subprocess.run([str(tool), 'extractcd', '-i', str(chd),
                                 '-o', str(staging / (stem + '.cue')), '-sb'],
                                capture_output=True, text=True, errors='replace')
        if result.returncode:
            tail = '\n'.join((result.stdout + result.stderr).splitlines()[-12:])
            raise ValueError(f'chdman could not extract {chd}\n{tail}')
        apply_redump_names(staging / (stem + '.cue'), stem)
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            staging.rename(target)
        except OSError:
            # Another process finished the same container first; reuse its set.
            if not cue.is_file():
                raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return cue


def resolve_disc(disc, *, cache=None, chdman=None, stem=None) -> Path:
    """Return a .cue for `disc`, extracting a .chd container first if needed."""
    disc = Path(disc)
    if disc.suffix.lower() != '.chd':
        return disc
    return extract_chd(disc, cache=cache, chdman=chdman, stem=stem)


def is_msys_bash(path) -> bool:
    """Reject WSL's C:\\Windows\\System32\\bash.exe and anything else non-MSYS."""
    path = Path(path)
    if not path.is_file():
        return False
    system = Path(os.environ.get('SystemRoot') or 'C:/Windows')
    try:
        if path.resolve().is_relative_to(system.resolve()):
            return False
    except OSError:
        return False
    for argv, marker in (([str(path), '-c', 'echo $MSYSTEM'], MSYSTEM_MARKER),
                         ([str(path), '--version'], BASH_BUILD_MARKER)):
        try:
            output = subprocess.run(argv, capture_output=True, text=True,
                                    errors='replace', timeout=60).stdout
        except (OSError, subprocess.SubprocessError):
            return False
        if marker.search(output or ''):
            return True
    return False


def bash_candidates() -> list[Path]:
    """Git install roots, nearest first, as both bash locations under each."""
    roots = []
    try:
        exec_path = subprocess.check_output(['git', '--exec-path'], text=True, timeout=60).strip()
    except (OSError, subprocess.SubprocessError):
        exec_path = ''
    if exec_path:
        # C:/Program Files/Git/mingw64/libexec/git-core -> .../Git
        roots += list(Path(exec_path).parents)
    found = shutil.which('git')
    if found:
        # Works from cmd/PowerShell (Git/cmd/git.exe) and from Git Bash, where
        # git resolves to Git/mingw64/bin/git.exe and parents[1] is not the root.
        parents = Path(found).resolve().parents
        roots += [parents[index] for index in (1, 2) if index < len(parents)]
    candidates = []
    for root in roots:
        for relative in BASH_RELATIVE:
            candidate = root / relative
            if candidate not in candidates:
                candidates.append(candidate)
    return candidates


def find_git_bash() -> Path:
    """Git for Windows bash, for the BIOS fingerprint script and the CMake define."""
    global _bash
    if _bash is not None:
        return _bash
    explicit = os.environ.get('PSX_GIT_BASH')
    if explicit:
        if not is_msys_bash(explicit):
            raise ValueError(f'PSX_GIT_BASH is not a Git for Windows bash.exe: {explicit}')
        _bash = Path(explicit).resolve()
        return _bash
    for candidate in bash_candidates():
        if is_msys_bash(candidate):
            _bash = candidate.resolve()
            return _bash
    raise ValueError('Git for Windows bash is required for BIOS fingerprint verification; '
                     'install Git for Windows or set PSX_GIT_BASH')
