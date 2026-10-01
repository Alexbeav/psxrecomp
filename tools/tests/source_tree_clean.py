#!/usr/bin/env python3
"""Did the test suite leave the source tree as it found it?

Two commands, registered as the first and the last test of a suite
(runtime/source_tree_check.cmake):

  snapshot --root <tree> --state <file>   record what `git status` lists for the tree
  compare  --root <tree> --state <file>   list it again; fail on any difference

Why (PS1B-333, 2026-10-01): a test ran the real licence step with the
repository's own root as the product folder and left a licenses/ folder there,
39 files. Every test passed. The folder was found only because the final run
of the candidate counted `git status` lines by hand afterwards.

What is compared: every entry `git status --untracked-files=all` lists below
the tree (changed, added, removed and untracked files; ignored files never),
with the SHA-256 of each file. So a new file, a file changed again, and a file
that is gone are all differences. A tree that was not clean before the suite is
fine: only what changed during the suite counts.

It never blocks a suite it cannot judge. Without git, outside a git checkout
(a source package has none), or without a snapshot from this run, `compare`
says NOT CHECKED with the reason and exits 0. The line it prints on a pass
names the two counts, so a pass that examined nothing cannot look like one that
did.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

GIT_TIMEOUT_S = 300
SHOWN = 50


def run_git(root, *arguments):
    """(stdout bytes, '') or (None, why not)."""
    command = ['git', '--no-optional-locks', '-C', root] + list(arguments)
    try:
        done = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=GIT_TIMEOUT_S)
    except (OSError, subprocess.SubprocessError) as exc:
        return None, 'git did not run (%s)' % exc
    if done.returncode != 0:
        said = done.stderr.decode('utf-8', errors='replace').strip().splitlines()
        return None, 'git %s exit %d (%s)' % (arguments[0], done.returncode, said[0] if said else 'no message')
    return done.stdout, ''


def file_state(path):
    if os.path.isdir(path):
        return 'folder'
    try:
        digest = hashlib.sha256()
        with open(path, 'rb') as handle:
            for block in iter(lambda: handle.read(1 << 20), b''):
                digest.update(block)
        return digest.hexdigest()
    except OSError:
        return 'absent'


def entries(root):
    """({path: [status, content]}, '') for what git status lists below `root`, or (None, why not).

    Paths are relative to the checkout's top, '/' separated, as git prints them.
    """
    top, why = run_git(root, 'rev-parse', '--show-toplevel')
    if top is None:
        return None, why
    top = top.decode('utf-8', errors='replace').strip()
    listed, why = run_git(root, 'status', '--porcelain=v1', '-z', '--untracked-files=all', '--', '.')
    if listed is None:
        return None, why
    found = {}
    records = listed.decode('utf-8', errors='replace').split('\0')
    index = 0
    while index < len(records):
        record = records[index]
        index += 1
        if len(record) < 4:
            continue
        status, path = record[:2], record[3:]
        if 'R' in status or 'C' in status:
            index += 1                      # the next record is the path it came from
        found[path] = [status, file_state(os.path.join(top, *path.split('/')))]
    return found, ''


def cmd_snapshot(args):
    found, why = entries(args.root)
    state = {'root': os.path.abspath(args.root), 'taken': time.time(), 'entries': found, 'why': why}
    try:
        folder = os.path.dirname(os.path.abspath(args.state))
        if not os.path.isdir(folder):
            os.makedirs(folder)
        with open(args.state, 'w', encoding='utf-8') as handle:
            json.dump(state, handle)
    except OSError as exc:
        print('source tree: snapshot NOT WRITTEN (%s); the comparison will say NOT CHECKED' % exc)
        return 0
    if found is None:
        print('source tree: NOT CHECKED: %s' % why)
    else:
        print('source tree snapshot: %d entries listed by git status in %s' % (len(found), state['root']))
    return 0


def cmd_compare(args):
    try:
        with open(args.state, 'r', encoding='utf-8') as handle:
            state = json.load(handle)
    except (OSError, ValueError):
        print('source tree: NOT CHECKED: no snapshot from this run (%s). Run the whole suite: the '
              'snapshot is its first test.' % args.state)
        return 0
    try:
        os.remove(args.state)               # one snapshot, one comparison
    except OSError:
        pass
    before = state.get('entries')
    if before is None:
        print('source tree: NOT CHECKED: the snapshot could not list the tree: %s' % state.get('why'))
        return 0
    after, why = entries(args.root)
    if after is None:
        print('source tree: NOT CHECKED: %s' % why)
        return 0
    age = int(time.time() - float(state.get('taken') or 0))
    differences = (['new      %s' % path for path in sorted(after) if path not in before]
                   + ['changed  %s' % path for path in sorted(after)
                      if path in before and after[path] != before[path]]
                   + ['gone     %s' % path for path in sorted(before) if path not in after])
    where = os.path.abspath(args.root)
    if not differences:
        print('source tree unchanged by the suite: %d entries listed by git status before, %d after '
              '(%s; snapshot %d s earlier)' % (len(before), len(after), where, age))
        return 0
    print('source tree CHANGED by the suite: %d differences in %s (%d entries before, %d after; '
          'snapshot %d s earlier)' % (len(differences), where, len(before), len(after), age))
    for line in differences[:SHOWN]:
        print('  ' + line)
    if len(differences) > SHOWN:
        print('  ... and %d more' % (len(differences) - SHOWN))
    print('A test wrote into the source tree, or the tree was edited while the suite ran. A test '
          'writes into a temporary folder. "gone" also covers a listed file that is clean again.')
    return 1


def main(argv=None):
    try:
        sys.stdout.reconfigure(errors='replace')        # a path the console cannot print is no failure
    except (AttributeError, ValueError):
        pass
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command')
    for name, function in (('snapshot', cmd_snapshot), ('compare', cmd_compare)):
        sub = commands.add_parser(name)
        sub.add_argument('--root', required=True, help='the tree to watch: a folder of a git checkout')
        sub.add_argument('--state', required=True, help='the file that carries the snapshot to the comparison')
        sub.set_defaults(function=function)
    args = parser.parse_args(argv)
    if not getattr(args, 'function', None):
        parser.error('a command is required: snapshot or compare')
    return args.function(args)


if __name__ == '__main__':
    sys.exit(main())
