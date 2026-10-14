#!/usr/bin/env python3
"""Install a persistent pre-push guard without replacing another hook policy."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

# Only Eagle keeps its system drive C: small, so a guarded clone there belongs on another drive.
# On other hosts C: may be the work drive (Pegasus builds on its SSD, which is C:).
SMALL_SYSTEM_DRIVE_HOSTS = frozenset({'EAGLE'})


def refuses_drive_c():
    """True on a Windows host whose drive C: must not hold a guarded clone."""
    return sys.platform == 'win32' and os.environ.get('COMPUTERNAME', '').upper() in SMALL_SYSTEM_DRIVE_HOSTS


def check_hook_drive(hooks):
    if refuses_drive_c() and hooks.drive.upper() == 'C:':
        raise ValueError('refusing to write a hook on C:')


def install():
    configured = subprocess.run(['git', 'config', '--get', 'core.hooksPath'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if configured.returncode != 1:
        raise ValueError('existing hook path policy: integrate its pre-push hook explicitly')
    result = subprocess.run(['git', 'rev-parse', '--git-path', 'hooks'], check=True,
                            text=True, encoding='utf-8', errors='replace', stdout=subprocess.PIPE)
    hooks = Path(result.stdout.strip()).resolve()
    common = subprocess.run(['git', 'rev-parse', '--git-common-dir'], check=True,
                            text=True, encoding='utf-8', errors='replace', stdout=subprocess.PIPE)
    git_dir = Path(common.stdout.strip()).resolve()
    if not hooks.is_relative_to(git_dir):
        raise ValueError('hook directory resolves outside this clone Git directory')
    check_hook_drive(hooks)
    hook = hooks / 'pre-push'
    payload = hooks / 'public-commit-guard'
    for target in (hook, payload, payload / 'check_public_commits.py', payload / 'public_commit_identities.json'):
        if not target.resolve().is_relative_to(hooks):
            raise ValueError('hook target resolves outside this clone hook directory')
    python = shlex.quote(Path(sys.executable).as_posix())
    script = shlex.quote((payload / 'check_public_commits.py').as_posix())
    expected = '#!/bin/sh\n# public-commit-guard v1\n' + f'exec {python} -B {script} --pre-push "$@"\n'
    if hook.exists() and hook.read_text(encoding='utf-8') != expected:
        raise ValueError('existing pre-push hook: preserve it and integrate explicitly')
    payload.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent
    for name in ('check_public_commits.py', 'public_commit_identities.json'):
        shutil.copyfile(source / name, payload / name)
    if not hook.exists():
        with hook.open('x', encoding='utf-8', newline='\n') as output:
            output.write(expected)
    hook.chmod(0o755)
    print('Installed persistent public commit guard in this clone')


if __name__ == '__main__':
    try:
        install()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print('Guard installation refused: ' + str(exc), file=sys.stderr)
        sys.exit(1)
