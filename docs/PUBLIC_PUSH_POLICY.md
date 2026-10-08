# Public push checks

`tools/check_public_commits.py` owns the fork's public commit checks. It requires
Python 3 and Git. It checks every commit outside the accepted pin H's history,
including merge-parent diffs and markers removed in later commits. Fixes branched
from older source use the same history exemption. Historical pin H ancestors are
left unchanged. It refuses unlisted authors or committers, attribution, tool names
in commit messages, and the lifted reference marker in commit messages or diffs.

The reviewed identity list is `tools/public_commit_identities.json`. It currently
contains Alex's two recorded identities. A named outside contributor requires a
reviewed addition to this list before their new commits can pass. Do not generate
this list from commit history. Local work uses Alexandros Mandravillis and
Alexbeav@live.com for both author and committer.

Run `python -B tools/check_public_commits.py HEAD` from the repository root before
the pin gates. The historical pin is fixed in the check; its CLI has no override.
A missing base commit, invalid ref or failed verification refuses
the check. Fetch the accepted pin object first when using a shallow clone.

For a clone without an existing hook policy, run
`python -B tools/install_public_push_guard.py` from that clone. It copies the check
and identity list beside the clone's Git hooks and binds the current Python path.
The guard survives branch changes. Re-run the installer to update its copied
policy. It refuses writes on C: on Windows and refuses to replace an existing
hook or configured hook path. A modified installed hook is preserved, and a
hook-directory symlink outside the clone's Git directory is refused. Integrate
an existing hook explicitly. GitHub destinations,
including SSH aliases, are checked before a push; private Gitea destinations are
not restricted by this hook. New branches and every ref in a multi-ref push are
checked. Deleting a ref does not introduce a commit. Git's `--no-verify` can bypass
a local hook, so the fork CI repeats the commit check; the hook is a mistake guard,
not an access control system.

The protocol follows [Git's pre-push documentation](https://git-scm.com/docs/githooks#_pre_push).
