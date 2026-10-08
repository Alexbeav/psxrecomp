#!/usr/bin/env python3
"""Check fork commits and implement Git's pre-push input protocol."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import urlsplit

PIN_H = 'a279b5e86a8d4f027ccdc4965d2aabc39bfbf04e'
MARKER = ('LIFTED' + '-REWRITE').encode()
ATTRIBUTION = re.compile(rb'^\s*(?:co-authored-by|generated-by|assisted-by)\s*:', re.I | re.M)
TOOL_NAME = re.compile(rb'\b(?:codex|claude|chatgpt|openai|anthropic|copilot)\b', re.I)
POLICY = Path(__file__).with_name('public_commit_identities.json')


def git(*args):
    return subprocess.run(['git', *args], check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def github_remote(location):
    """Parse actual hosts; a path containing github.com is not a GitHub remote."""
    if '://' in location:
        parts = urlsplit(location)
        host = parts.hostname
        ssh_target = ((parts.username + '@') if parts.username else '') + (host or '')
        ssh_port = ['-p', str(parts.port)] if parts.port else []
    else:
        match = re.fullmatch(r'((?:[^/@:]+@)?)([^/:]+):.+', location)
        host = match.group(2) if match else None
        ssh_target = (match.group(1) + host) if match else ''
        ssh_port = []
    if not host or (len(host) == 1 and location[1:2] == ':'):
        return False
    if host.lower() in ('github.com', 'ssh.github.com'):
        return True
    # SSH aliases must resolve to their real destination before classification.
    if '://' not in location or location.startswith('ssh://'):
        resolved = subprocess.run(['ssh', '-G', *ssh_port, ssh_target], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, check=True).stdout
        for line in resolved.decode('utf-8', 'replace').splitlines():
            if line.startswith('hostname '):
                return line.split(maxsplit=1)[1].lower() in ('github.com', 'ssh.github.com')
    return False


def check_refs(refs, base=PIN_H):
    allowed = set(json.loads(POLICY.read_text(encoding='utf-8')))
    base = git('rev-parse', '--verify', '--end-of-options', base + '^{commit}').strip().decode('ascii')
    tips = [git('rev-parse', '--verify', '--end-of-options', ref + '^{commit}').strip().decode('ascii') for ref in refs]
    commits = git('rev-list', *tips, '--not', base).decode('ascii').splitlines()
    errors = []
    for commit in commits:
        raw = git('show', '-s', '--format=%an <%ae>%x00%cn <%ce>%x00%B', commit)
        author, committer, message = raw.split(b'\0', 2)
        for role, identity in (('author', author), ('committer', committer)):
            if identity.decode('utf-8', 'replace') not in allowed:
                errors.append(f'{commit}: {role} is outside the reviewed identity list')
        if ATTRIBUTION.search(message) or TOOL_NAME.search(message):
            errors.append(f'{commit}: message has attribution or a tool name')
        # Separate merge-parent diffs, root diffs, deletion and rename content all count.
        diff = git('show', '--format=', '--root', '--diff-merges=separate', '--no-color',
                   '--no-ext-diff', '--no-textconv', '--no-renames', '--text', commit)
        if MARKER in message or MARKER in diff:
            errors.append(f'{commit}: lifted reference marker occurs in message or diff')
    if errors:
        raise ValueError('\n'.join(errors))
    print(f'Public commit check: {len(commits)} commits passed')


def pre_push(location, lines, base=PIN_H):
    if not github_remote(location):
        return
    tips = []
    for line in lines:
        fields = line.split()
        if len(fields) != 4:
            raise ValueError('invalid pre-push ref record')
        _, local_oid, _, remote_oid = fields
        if not re.fullmatch(r'[0-9a-fA-F]{40}|[0-9a-fA-F]{64}', local_oid):
            raise ValueError('invalid local object id')
        if len(local_oid) != len(remote_oid) or not re.fullmatch(r'[0-9a-fA-F]+', remote_oid):
            raise ValueError('invalid remote object id')
        if set(local_oid) != {'0'}:
            tips.append(local_oid)
    if tips:
        # Check full post-pin ancestry, including a marker removed by a later commit.
        # Remote refs cannot hide an already-public bad ancestor or a newly named branch.
        check_refs(tips, base)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pre-push', nargs=2, metavar=('REMOTE', 'LOCATION'))
    parser.add_argument('refs', nargs='*', help='commit refs to check; default HEAD')
    args = parser.parse_args(argv)
    try:
        if args.pre_push:
            pre_push(args.pre_push[1], sys.stdin)
        else:
            check_refs(args.refs or ['HEAD'])
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        # Never echo the remote location: an HTTPS URL can contain a credential.
        if isinstance(exc, subprocess.CalledProcessError):
            detail = 'required Git/SSH verification failed; no public push admitted'
        else:
            detail = str(exc)
        print('Public commit check refused: ' + detail, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
