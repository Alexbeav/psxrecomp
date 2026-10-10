"""Accept .chd disc containers without changing how media is verified.

A .chd input is extracted with chdman into a private cache directory keyed by
the container's own SHA-256, split into one .bin per track, and renamed to the
original redump track layout. The resolved .cue is then handed to the caller's
existing pinned size/hash check, which admits or rejects it exactly as it would
a .cue the operator passed directly: nothing here compares, relaxes or
substitutes an expected identity, and a container whose contents differ fails
with the same error a wrong .cue produces. A non-.chd path is returned
untouched, so .cue input keeps its current code path byte for byte.

This module uses tools/host_bash.py for the Git for Windows shell used for
tools/bios_emitter_fingerprint.sh and the -D_psxrt_bash= CMake define.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import host_bash

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
# Track files as chdman writes them into the cue it generates for a split dump.
CUE_FILE = re.compile(rb'FILE\s+"([^"\r\n]+)"\s+BINARY', re.I)
# Searched only after --chdman, PSX_CHDMAN and PATH have all come up empty.
CHDMAN_FALLBACKS = (ROOT / 'tools/mame-0.289/chdman.exe',
                    Path('D:/psxrecomp/tools/mame-0.289/chdman.exe'))
# Git for Windows ships the MSYS bash at both of these, relative to its root.
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
    """Compatibility adapter for the shared MSYS validation."""
    return host_bash.is_msys_bash(str(path))


def find_git_bash() -> Path:
    """Keep the replay API and cache; selection belongs to host_bash."""
    global _bash
    if _bash is None:
        try:
            _bash = Path(host_bash.find_bash("BIOS fingerprint verification",
                                           windows=True, verify_msys=True)).resolve()
        except AssertionError as exc:
            raise ValueError(str(exc)) from exc
    return _bash
