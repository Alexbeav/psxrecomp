"""Clean-source check shared by the route setups (tekken3, pepsiman, crash, abesoddysee, biohazard,
redc, megamanx4, megamanx5).

A setup refuses to build from, and refuses a build that changed, a source tree with uncommitted
changes. The check leaves out generated/ only. Setup itself writes there: the BIOS C it emits and
generated/<stem>.emitter.sha, whose fingerprint hashes the project's bios.toml, and that profile
embeds the absolute project paths. A setup in any new project directory therefore rewrites a
tracked stamp, and the tree was never clean afterwards.

Leaving generated/ out does not drop its identity: each setup records the SHA-256 of every
generated file it builds with (setup.json 'generated', the build-cache generated set, or the
tekken3-codegen.json text check), and those checks are unchanged. Every other tracked or untracked
path, including bios/ profiles, seeds, recompiler/ and runtime/, still makes the tree dirty.
"""
import hashlib
import subprocess
from pathlib import Path

EXCLUDED = 'generated'


def status_command(root):
    return ['git', '-C', str(root), 'status', '--porcelain', '--', '.', ':(exclude)' + EXCLUDED]


def dirty(root):
    """git status --porcelain of the source tree outside generated/; empty when clean."""
    return subprocess.check_output(status_command(root), encoding='utf-8', errors='replace').strip()


def generated_record(root, stem, project):
    """SHA-256 of every generated file a setup builds with, in the form the other setups record:
    <root>/generated/<stem>* by repository-relative path, <project>/generated/* by full path."""
    root, project = Path(root), Path(project)
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    record = {str(f.relative_to(root)): digest(f) for f in sorted((root / EXCLUDED).glob(stem + '*')) if f.is_file()}
    record.update({str(f): digest(f) for f in sorted((project / 'generated').glob('*')) if f.is_file()})
    return record
