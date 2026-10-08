#!/usr/bin/env python3
"""Private Git object fixtures; no network or public refs are changed."""
from contextlib import redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import shlex

SPEC = importlib.util.spec_from_file_location('guard', Path(__file__).with_name('check_public_commits.py'))
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)
ALEX = ('Alexandros Mandravillis', 'Alexbeav@live.com')
BAD = ('unlisted fixture', 'fixture@example.invalid')
INSTALLER = Path(__file__).with_name('install_public_push_guard.py').resolve()
CHECKER = Path(__file__).with_name('check_public_commits.py').resolve()


def run(*args, data=None, env=None):
    return subprocess.run(['git', *args], input=data, env=env, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def fixture_parent():
    """Return the folder that holds the fixtures, or None for the default.

    The installer refuses a hook folder on C: on Windows, and the default
    temporary folder is on C: there. The working directory is used then,
    because ctest starts this file in the build tree.
    """
    if sys.platform == 'win32' and Path(tempfile.gettempdir()).resolve().drive.upper() == 'C:':
        here = Path.cwd().resolve()
        if here.drive.upper() != 'C:':
            return str(here)
    return None


def report(result):
    """Describe an installer run for an assertion message."""
    return 'installer exit {}\nfixture: {}\nstdout: {}\nstderr: {}'.format(
        result.returncode, Path.cwd(), result.stdout.decode('utf-8', 'replace').strip(),
        result.stderr.decode('utf-8', 'replace').strip())


FIXTURE_PARENT = fixture_parent()


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='public-commit-guard-', dir=FIXTURE_PARENT)
        self.previous = Path.cwd()
        os.chdir(self.folder.name)
        # A fixture below a work tree must never reach the repository above it.
        ceiling = patch.dict(os.environ, GIT_CEILING_DIRECTORIES=str(Path(self.folder.name).resolve().parent))
        ceiling.start()
        self.addCleanup(ceiling.stop)
        run('init', '--quiet')
        self.base = self.commit('historical', author=BAD, committer=BAD)

    def tearDown(self):
        os.chdir(self.previous)
        self.folder.cleanup()

    def commit(self, message, *, parent=None, author=ALEX, committer=ALEX, content=b'fixture\n', parents=None):
        blob = run('hash-object', '-w', '--stdin', data=content).strip()
        tree = run('mktree', data=b'100644 blob ' + blob + b'\tfixture\n').strip().decode()
        args = ['commit-tree', tree]
        for ancestor in parents if parents is not None else ([parent] if parent else []):
            args += ['-p', ancestor]
        env = dict(os.environ, GIT_AUTHOR_NAME=author[0], GIT_AUTHOR_EMAIL=author[1],
                   GIT_COMMITTER_NAME=committer[0], GIT_COMMITTER_EMAIL=committer[1],
                   GIT_AUTHOR_DATE='2026-10-07T12:00:00+00:00', GIT_COMMITTER_DATE='2026-10-07T12:00:00+00:00')
        return run(*args, data=message.encode(), env=env).strip().decode()

    def check(self, *refs):
        with redirect_stdout(io.StringIO()):
            guard.check_refs(list(refs), self.base)

    def push(self, *refs, location='https://github.com/fixture/fixture.git'):
        lines = [f'refs/heads/test{i} {ref} refs/heads/test{i} ' + '0' * 40 for i, ref in enumerate(refs)]
        with redirect_stdout(io.StringIO()):
            guard.pre_push(location, lines, self.base)

    def test_historical_bad_identity_is_exempt(self):
        self.check(self.commit('valid', parent=self.base))

    def test_author_and_committer_are_checked_separately(self):
        for field in ('author', 'committer'):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field):
                self.check(self.commit('valid', parent=self.base, **{field: BAD}))

    def test_second_recorded_identity(self):
        other = (ALEX[0], 'siralexbeav@gmail.com')
        self.check(self.commit('valid', parent=self.base, author=other, committer=other))

    def test_attribution_and_tool_messages(self):
        for message in ('valid\n\nCo-Authored-By: fixture', 'generated-by: fixture', 'fix with Codex'):
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, 'message'):
                self.check(self.commit(message, parent=self.base))

    def test_marker_in_message_or_diff(self):
        for message, content in ((guard.MARKER.decode(), b'clean'), ('valid', guard.MARKER),
                                 ('valid', b'\0' + guard.MARKER + b'\0')):
            with self.subTest(content=content), self.assertRaisesRegex(ValueError, 'marker'):
                self.check(self.commit(message, parent=self.base, content=content))

    def test_removed_marker_still_refused_on_new_branch(self):
        marked = self.commit('private', parent=self.base, content=guard.MARKER)
        clean = self.commit('remove private content', parent=marked)
        with self.assertRaisesRegex(ValueError, 'marker'):
            self.push(clean)

    def test_merge_checks_nonfirst_parent(self):
        clean = self.commit('valid', parent=self.base)
        bad = self.commit('valid', parent=self.base, committer=BAD)
        merge = self.commit('merge', parents=[clean, bad])
        with self.assertRaisesRegex(ValueError, 'committer'):
            self.check(merge)

    def test_merge_diff_checks_both_parents(self):
        marked = self.commit('private', parent=self.base, content=guard.MARKER)
        clean = self.commit('valid', parent=self.base)
        merge = self.commit('merge', parents=[clean, marked])
        with self.assertRaisesRegex(ValueError, 'marker'):
            self.check(merge)

    def test_multiref_checks_every_tip(self):
        clean = self.commit('valid', parent=self.base)
        bad = self.commit('valid', parent=self.base, author=BAD)
        with self.assertRaisesRegex(ValueError, 'author'):
            self.push(clean, bad)

    def test_private_remote_allows_private_commits(self):
        marked = self.commit('private', parent=self.base, content=guard.MARKER, author=BAD)
        self.push(marked, location='https://gitea.example.invalid/private/fixture.git')

    def test_delete_does_not_introduce_commit(self):
        self.push('0' * 40)

    def test_invalid_protocol_refused(self):
        for line in ('too few fields', 'local not-an-oid remote ' + '0' * 40):
            with self.subTest(line=line), self.assertRaises(ValueError):
                guard.pre_push('https://github.com/fixture/fixture', [line], self.base)

    def test_missing_baseline_refused(self):
        with self.assertRaises(subprocess.CalledProcessError):
            guard.check_refs([self.base], 'f' * 40)

    def test_older_source_branch_preserves_historical_exemption(self):
        historical = self.base
        self.base = self.commit('accepted historical pin', parent=historical)
        self.check(self.commit('maintain earlier source', parent=historical))

    def test_older_source_branch_checks_its_new_commits(self):
        historical = self.base
        self.base = self.commit('accepted historical pin', parent=historical)
        sibling = self.commit('maintain earlier source', parent=historical, author=BAD)
        with self.assertRaisesRegex(ValueError, 'author'):
            self.check(sibling)

    def test_remote_authority(self):
        for location in ('https://github.com/fixture/x', 'ssh://git@ssh.github.com:443/fixture/x', 'git@github.com:fixture/x'):
            self.assertTrue(guard.github_remote(location))
        for location in ('https://github.com.example.invalid/x', 'https://example.invalid/github.com/x', 'D:/github.com/x'):
            self.assertFalse(guard.github_remote(location))
        result = subprocess.CompletedProcess([], 0, b'hostname github.com\n', b'')
        with patch.object(guard.subprocess, 'run', return_value=result) as ssh:
            self.assertTrue(guard.github_remote('git@fork-alias:fixture/x'))
            self.assertEqual(ssh.call_args.args[0], ['ssh', '-G', 'git@fork-alias'])

    def install(self):
        return subprocess.run([sys.executable, '-B', str(INSTALLER)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)

    def installed(self):
        result = self.install()
        self.assertEqual(result.returncode, 0, report(result))

    def refused(self, reason):
        result = self.install()
        self.assertNotEqual(result.returncode, 0, report(result))
        self.assertIn(reason, result.stderr.decode('utf-8', 'replace'), report(result))

    def test_installer_preserves_existing_hook_and_path(self):
        hook = Path('.git/hooks/pre-push')
        original = b'#!/bin/sh\necho existing fixture hook\n'
        hook.write_bytes(original)
        self.refused('existing pre-push hook')
        self.assertEqual(hook.read_bytes(), original)
        hook.unlink()
        run('config', 'core.hooksPath', 'other-hooks')
        self.refused('existing hook path policy')
        self.assertEqual(run('config', '--get', 'core.hooksPath').strip(), b'other-hooks')

    def test_cli_cannot_override_historical_base(self):
        run('update-ref', 'refs/heads/base-fixture', self.base)
        run('symbolic-ref', 'HEAD', 'refs/heads/base-fixture')
        result = subprocess.run([sys.executable, '-B', str(CHECKER), '--base', 'HEAD'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 2)

    def test_installer_preserves_modified_own_hook(self):
        self.installed()
        hook = Path('.git/hooks/pre-push')
        modified = hook.read_bytes() + b'echo user hook modification\n'
        hook.write_bytes(modified)
        self.refused('existing pre-push hook')
        self.assertEqual(hook.read_bytes(), modified)

    def test_installer_rejects_symlinked_hook_directory(self):
        hooks = Path('.git/hooks')
        saved = Path('.git/saved-hooks')
        outside = Path('outside-hooks').resolve()
        self.assertTrue(hooks.resolve().is_relative_to(Path.cwd()))
        self.assertTrue(saved.resolve().is_relative_to(Path.cwd()))
        hooks.rename(saved)
        outside.mkdir()
        try:
            hooks.symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest('directory symlink privilege unavailable')
        self.refused('hook directory resolves outside this clone Git directory')
        self.assertEqual(list(outside.iterdir()), [])

    def test_installed_hook_blocks_actual_mock_public_push_and_allows_private(self):
        self.installed()
        self.assertTrue(Path('.git/hooks/pre-push').is_file())
        self.guarded_pushes()

    def test_installer_in_linked_work_tree_uses_the_shared_hook_folder(self):
        hooks = Path('.git/hooks').resolve()
        linked = Path('linked-work-tree').resolve()
        run('worktree', 'add', '--detach', str(linked), self.base)
        os.chdir(linked)
        self.assertTrue(Path('.git').is_file())
        self.installed()
        self.assertTrue((hooks / 'pre-push').is_file())
        self.assertTrue((hooks / 'public-commit-guard' / 'check_public_commits.py').is_file())
        self.guarded_pushes()

    def guarded_pushes(self):
        """Push through the installed hook from the current work tree."""
        hooks = Path(run('rev-parse', '--git-path', 'hooks').decode('utf-8').strip()).resolve()
        copied = hooks / 'public-commit-guard' / 'check_public_commits.py'
        # The isolated transport fixture uses its own historical pin. Production
        # pin identity is tested separately by invoking the guard on the stage head.
        copied.write_text(copied.read_text(encoding='utf-8').replace(guard.PIN_H, self.base), encoding='utf-8')
        remote = Path('private-remote.git').resolve()
        run('init', '--bare', '--quiet', str(remote))
        ssh = Path('local-transport.py').resolve()
        ssh.write_text("import os,subprocess,sys\nsys.exit(subprocess.run(['git','receive-pack',os.environ['GUARD_FIXTURE_REMOTE']]).returncode)\n", encoding='utf-8')
        env = dict(os.environ, GIT_SSH_VARIANT='ssh', GUARD_FIXTURE_REMOTE=str(remote),
                   GIT_SSH_COMMAND=shlex.quote(Path(sys.executable).as_posix()) + ' -B ' + shlex.quote(ssh.as_posix()))
        marked = self.commit('private fixture', parent=self.base, content=guard.MARKER)
        pushed = subprocess.run(['git', 'push', 'git@github.com:fixture/fixture.git', marked + ':refs/heads/rejected'],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(pushed.returncode, 0, pushed.stderr)
        self.assertIn(b'lifted reference marker', pushed.stderr)
        absent = subprocess.run(['git', '--git-dir', str(remote), 'show-ref', '--verify', 'refs/heads/rejected'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(absent.returncode, 0)
        # A local private receive-pack control receives the exact same commit.
        run('push', str(remote), marked + ':refs/heads/private-fixture')
        self.assertEqual(run('--git-dir', str(remote), 'rev-parse', 'refs/heads/private-fixture').strip().decode(), marked)
        clean = self.commit('valid fixture', parent=self.base)
        good = subprocess.run(['git', 'push', 'git@github.com:fixture/fixture.git', clean + ':refs/heads/valid'],
                              env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertEqual(run('--git-dir', str(remote), 'rev-parse', 'refs/heads/valid').strip().decode(), clean)

    def test_installer_refuses_symlink_escape(self):
        outside = Path(self.folder.name).parent / (Path(self.folder.name).name + '-outside')
        payload = Path('.git/hooks/public-commit-guard')
        try:
            payload.symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest('directory symlink privilege unavailable')
        self.refused('hook target resolves outside this clone hook directory')
        self.assertFalse(outside.exists())


if __name__ == '__main__':
    unittest.main()
