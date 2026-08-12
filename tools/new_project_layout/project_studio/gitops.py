"""Git / GitHub operations for a game repository.

Uses ``git`` and (optionally) ``gh``. No force-push, amend, or hook skips.
"""

from __future__ import annotations

import configparser
import json
import re
import shutil
import subprocess
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

DEFAULT_PSXRECOMP_URL = "https://github.com/mstan/psxrecomp.git"
DEFAULT_RECOMP_UI_URL = "https://github.com/mstan/recomp-ui.git"
DEFAULT_RECOMP_NET_URL = "https://github.com/TechnicallyComputers/recomp-net.git"
DEFAULT_RBENGINE_URL = "https://github.com/TechnicallyComputers/retcomm-rbengine.git"
DEFAULT_BRANCH = "master"
DEFAULT_NESTED_BRANCH = "main"
KNOWN_SUBMODULES = ("psxrecomp", "recomp-ui")
# Nested under a psxrecomp checkout (game/psxrecomp or the engine repo itself).
KNOWN_NESTED_SUBMODULES: tuple[tuple[str, str, str], ...] = (
    ("lib/recomp-net", DEFAULT_RECOMP_NET_URL, DEFAULT_NESTED_BRANCH),
    ("lib/retcomm-rbengine", DEFAULT_RBENGINE_URL, DEFAULT_NESTED_BRANCH),
)
NESTED_PATHS = tuple(p for p, _, _ in KNOWN_NESTED_SUBMODULES)


@dataclass
class CmdResult:
    ok: bool
    message: str
    detail: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class SubmoduleInfo:
    name: str
    path: str
    url: str = ""
    branch: str = ""
    sha: str = ""
    present: bool = False
    initialized: bool = False
    checkout_branch: str = ""  # actual HEAD branch; empty if detached / missing

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class RepoStatus:
    root: str
    is_git: bool
    branch: str = ""
    upstream: str = ""
    ahead: int = 0
    behind: int = 0
    dirty: bool = False
    staged: int = 0
    unstaged: int = 0
    untracked: int = 0
    remote_url: str = ""
    gh_available: bool = False
    gh_repo: str = ""
    short_status: str = ""
    submodules: list[SubmoduleInfo] = field(default_factory=list)
    nested_submodules: list[SubmoduleInfo] = field(default_factory=list)
    psxrecomp_root: str = ""
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["submodules"] = [s.to_dict() for s in self.submodules]
        d["nested_submodules"] = [s.to_dict() for s in self.nested_submodules]
        return d


def _run(
    cmd: list[str],
    cwd: Path,
    *,
    dry_run: bool = False,
    check: bool = False,
) -> tuple[int, str, str]:
    if dry_run:
        return 0, "dry-run: " + " ".join(cmd), ""
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        if check:
            raise
        return 127, "", str(exc)
    return proc.returncode, (proc.stdout or "").strip(), (proc.stderr or "").strip()


def _git(cwd: Path, *args: str, dry_run: bool = False) -> tuple[int, str, str]:
    return _run(["git", *args], cwd, dry_run=dry_run)


def current_branch(root: Path) -> str | None:
    """Return the checked-out branch name, or None if detached / unknown."""
    root = root.expanduser().resolve()
    if not root.is_dir():
        return None
    code, out, _ = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    if code != 0:
        return None
    name = out.strip()
    if not name or name == "HEAD":
        return None
    return name


def _which_gh() -> str | None:
    return shutil.which("gh")


def _is_git_repo(root: Path) -> bool:
    code, out, _ = _git(root, "rev-parse", "--is-inside-work-tree")
    return code == 0 and out.strip() == "true"


def _read_gitmodules(root: Path) -> configparser.ConfigParser:
    cp = configparser.ConfigParser(interpolation=None)
    gm = root / ".gitmodules"
    if gm.is_file():
        cp.read(gm, encoding="utf-8")
    return cp


def _write_gitmodules(root: Path, cp: configparser.ConfigParser, *, dry_run: bool) -> None:
    if dry_run:
        return
    buf: list[str] = []
    for section in cp.sections():
        buf.append(f"[{section}]")
        for key, value in cp.items(section):
            buf.append(f"\t{key} = {value}")
        buf.append("")
    text = "\n".join(buf).rstrip() + "\n"
    (root / ".gitmodules").write_text(text, encoding="utf-8")


def _section_for_path(cp: configparser.ConfigParser, path: str) -> str | None:
    for section in cp.sections():
        if cp.get(section, "path", fallback="") == path:
            return section
    # Common form: [submodule "psxrecomp"]
    want = f'submodule "{path}"'
    for section in cp.sections():
        if section == want or section.endswith(f'"{path}"'):
            return section
    return None


def _submodule_sha(root: Path, path: str) -> str:
    code, out, _ = _git(root, "rev-parse", f"HEAD:{path}")
    if code == 0 and re.fullmatch(r"[0-9a-f]{40}", out.strip()):
        return out.strip()[:12]
    # Fallback: ls-tree
    code, out, _ = _git(root, "ls-tree", "HEAD", path)
    if code == 0:
        parts = out.split()
        if len(parts) >= 3 and parts[0] == "160000":
            return parts[2][:12]
    # Working tree HEAD inside submodule
    sub = root / path
    if sub.is_dir():
        code, out, _ = _git(sub, "rev-parse", "HEAD")
        if code == 0:
            return out.strip()[:12]
    return ""


def _submodule_remote_url(root: Path, path: str) -> str:
    """Best-effort origin URL from a submodule working tree."""
    sub = root / path
    if not sub.is_dir():
        return ""
    code, out, _ = _git(sub, "remote", "get-url", "origin")
    if code == 0 and out.strip():
        return out.strip()
    return ""


def _default_url_for_path(path: str) -> str:
    for p, url, _ in KNOWN_NESTED_SUBMODULES:
        if p == path:
            return url
    if path == "psxrecomp":
        return DEFAULT_PSXRECOMP_URL
    if path == "recomp-ui":
        return DEFAULT_RECOMP_UI_URL
    return ""


def _list_submodules(root: Path, *, known: tuple[str, ...] = KNOWN_SUBMODULES) -> list[SubmoduleInfo]:
    cp = _read_gitmodules(root)
    found: dict[str, SubmoduleInfo] = {}
    for section in cp.sections():
        path = cp.get(section, "path", fallback="")
        if not path:
            continue
        name = path
        m = re.search(r'"([^"]+)"', section)
        if m:
            name = m.group(1)
        url = cp.get(section, "url", fallback="").strip()
        if not url:
            url = _submodule_remote_url(root, path) or _default_url_for_path(path)
        found[path] = SubmoduleInfo(
            name=name,
            path=path,
            url=url,
            branch=cp.get(section, "branch", fallback=""),
            sha=_submodule_sha(root, path),
            present=(root / path).exists(),
            initialized=(root / path / ".git").exists()
            or ((root / path).is_dir() and (root / ".git" / "modules" / path).exists()),
            checkout_branch=current_branch(root / path) or ""
            if (root / path).is_dir()
            else "",
        )
    # Ensure known slots show up even if missing from .gitmodules
    for path in known:
        if path not in found:
            url = _submodule_remote_url(root, path) or _default_url_for_path(path)
            present = (root / path).exists()
            found[path] = SubmoduleInfo(
                name=path,
                path=path,
                url=url,
                present=present,
                initialized=(root / path / ".git").exists(),
                sha=_submodule_sha(root, path) if present else "",
                checkout_branch=(current_branch(root / path) or "") if present else "",
            )
    # Stable order: known first, then others
    ordered: list[SubmoduleInfo] = []
    for path in known:
        if path in found:
            ordered.append(found.pop(path))
    ordered.extend(sorted(found.values(), key=lambda s: s.path))
    return ordered


def resolve_psxrecomp_dir(root: Path) -> Path | None:
    """Return the psxrecomp checkout: either ``root`` itself or ``root/psxrecomp``."""
    root = root.expanduser().resolve()
    if (root / "runtime" / "runtime.cmake").is_file():
        return root
    nested = root / "psxrecomp"
    if (nested / "runtime" / "runtime.cmake").is_file():
        return nested
    if nested.is_dir() and (nested / ".git").exists():
        return nested
    return None


def list_nested_modules(root: Path) -> list[SubmoduleInfo]:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return []
    return _list_submodules(psx, known=NESTED_PATHS)


def repo_status(root: Path) -> RepoStatus:
    root = root.expanduser().resolve()
    st = RepoStatus(root=str(root), is_git=_is_git_repo(root), gh_available=bool(_which_gh()))
    if not st.is_git:
        st.notes.append("Not a git repository.")
        return st

    code, branch, _ = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    st.branch = branch if code == 0 else ""

    code, upstream, _ = _git(root, "rev-parse", "--abbrev-ref", "@{upstream}")
    if code == 0:
        st.upstream = upstream
        code2, ab, _ = _git(root, "rev-list", "--left-right", "--count", f"{upstream}...HEAD")
        if code2 == 0:
            parts = ab.split()
            if len(parts) == 2:
                st.behind = int(parts[0])
                st.ahead = int(parts[1])

    code, porcelain, _ = _git(root, "status", "--porcelain")
    if code == 0:
        lines = [ln for ln in porcelain.splitlines() if ln.strip()]
        st.short_status = "\n".join(lines[:40])
        for ln in lines:
            xy = ln[:2]
            if ln.startswith("??"):
                st.untracked += 1
            else:
                if xy[0] not in (" ", "?"):
                    st.staged += 1
                if xy[1] not in (" ", "?"):
                    st.unstaged += 1
        st.dirty = bool(lines)

    code, url, _ = _git(root, "remote", "get-url", "origin")
    if code == 0:
        st.remote_url = url

    if st.gh_available:
        code, out, err = _run(
            ["gh", "repo", "view", "--json", "nameWithOwner", "-q", ".nameWithOwner"],
            root,
        )
        if code == 0 and out:
            st.gh_repo = out.strip()
        elif err:
            st.notes.append("gh present but not authenticated / no GitHub remote.")

    st.submodules = _list_submodules(root)
    psx = resolve_psxrecomp_dir(root)
    if psx is not None:
        st.psxrecomp_root = str(psx)
        if psx == root:
            # Engine checkout: top-level known slots are the nested libs.
            st.submodules = _list_submodules(root, known=NESTED_PATHS)
            st.nested_submodules = list(st.submodules)
            st.notes.append("Root is a psxrecomp checkout (nested modules are direct).")
        else:
            st.nested_submodules = list_nested_modules(root)
    return st


def set_submodule_url(
    root: Path,
    path: str,
    url: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    """Write ``url`` into ``.gitmodules`` for ``path`` (create section if needed)."""
    root = root.expanduser().resolve()
    url = url.strip()
    if not url:
        return CmdResult(False, "URL required")
    path = path.strip().replace("\\", "/")
    cp = _read_gitmodules(root)
    section = _section_for_path(cp, path)
    if section is None:
        if not (root / path).exists():
            return CmdResult(False, f"No .gitmodules entry for {path}")
        section = f'submodule "{path}"'
        cp.add_section(section)
        cp.set(section, "path", path)
    prev = cp.get(section, "url", fallback="").strip()
    if prev == url:
        return CmdResult(True, f"{path} url already set")
    cp.set(section, "url", url)
    _write_gitmodules(root, cp, dry_run=dry_run)
    if not dry_run:
        _git(root, "config", "-f", ".gitmodules", f"submodule.{path}.url", url)
        _git(root, "submodule", "sync", "--", path)
    return CmdResult(True, f"Set {path} url → {url}")


def ensure_submodule(
    root: Path,
    path: str,
    *,
    url: str,
    branch: str = DEFAULT_BRANCH,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    if not _is_git_repo(root):
        return CmdResult(False, "Not a git repository")
    dest = root / path
    cp = _read_gitmodules(root)
    already_registered = _section_for_path(cp, path) is not None
    looks_present = dest.exists() and (
        (dest / ".git").exists()
        or (dest / "CMakeLists.txt").is_file()
        or (dest / "runtime" / "runtime.cmake").is_file()
        or any(dest.iterdir())
    )
    if already_registered or looks_present:
        notes: list[str] = []
        # Heal missing .gitmodules url (common when a submodule was added by hand).
        section = _section_for_path(cp, path)
        have_url = ""
        if section:
            have_url = cp.get(section, "url", fallback="").strip()
        if not have_url and url:
            url_r = set_submodule_url(root, path, url, dry_run=dry_run)
            notes.append(url_r.message)
        set_r = set_submodule_branch(root, path, branch, dry_run=dry_run)
        notes.append(set_r.message)
        return CmdResult(
            True,
            f"{path} already present",
            "; ".join(n for n in notes if n),
        )

    code, out, err = _git(
        root,
        "submodule",
        "add",
        "-b",
        branch,
        url,
        path,
        dry_run=dry_run,
    )
    if code != 0:
        return CmdResult(False, f"Failed to add {path}", err or out)
    _git(root, "submodule", "update", "--init", "--recursive", path, dry_run=dry_run)
    return CmdResult(True, f"Added submodule {path} (branch {branch})", out)


def ensure_known_submodules(
    root: Path,
    *,
    psxrecomp_branch: str = DEFAULT_BRANCH,
    recomp_ui_branch: str = DEFAULT_BRANCH,
    dry_run: bool = False,
) -> list[CmdResult]:
    return [
        ensure_submodule(
            root,
            "psxrecomp",
            url=DEFAULT_PSXRECOMP_URL,
            branch=psxrecomp_branch or DEFAULT_BRANCH,
            dry_run=dry_run,
        ),
        ensure_submodule(
            root,
            "recomp-ui",
            url=DEFAULT_RECOMP_UI_URL,
            branch=recomp_ui_branch or DEFAULT_BRANCH,
            dry_run=dry_run,
        ),
    ]


def ensure_nested_modules(
    root: Path,
    *,
    recomp_net_branch: str = DEFAULT_NESTED_BRANCH,
    rbengine_branch: str = DEFAULT_NESTED_BRANCH,
    dry_run: bool = False,
) -> list[CmdResult]:
    """Ensure ``lib/recomp-net`` + ``lib/retcomm-rbengine`` inside psxrecomp."""
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return [CmdResult(False, "No psxrecomp checkout found (need root or root/psxrecomp)")]
    if not _is_git_repo(psx):
        return [CmdResult(False, f"psxrecomp is not a git repo: {psx}")]

    branch_by_path = {
        "lib/recomp-net": recomp_net_branch or DEFAULT_NESTED_BRANCH,
        "lib/retcomm-rbengine": rbengine_branch or DEFAULT_NESTED_BRANCH,
    }
    results: list[CmdResult] = []
    for path, url, default_branch in KNOWN_NESTED_SUBMODULES:
        results.append(
            ensure_submodule(
                psx,
                path,
                url=url,
                branch=branch_by_path.get(path, default_branch),
                dry_run=dry_run,
            )
        )
    return results


def update_nested_modules(
    root: Path,
    *,
    paths: list[str] | None = None,
    remote: bool = False,
    stage: bool = True,
    dry_run: bool = False,
) -> CmdResult:
    """Update nested modules inside psxrecomp; optionally stage gitlinks there."""
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    want = paths or list(NESTED_PATHS)
    # Allow callers to pass game-relative paths
    normalized: list[str] = []
    for p in want:
        p = p.strip().replace("\\", "/")
        if p.startswith("psxrecomp/"):
            p = p[len("psxrecomp/") :]
        normalized.append(p)

    r = update_submodules(psx, paths=normalized, remote=remote, dry_run=dry_run)
    if not r.ok:
        return r
    if stage and not dry_run:
        code, out, err = _git(psx, "add", "--", *normalized)
        if code != 0:
            return CmdResult(
                False,
                "Nested update ok but failed to stage gitlinks in psxrecomp",
                err or out,
            )
        detail = (r.detail + "\n" if r.detail else "") + "staged in psxrecomp: " + ", ".join(
            normalized
        )
        return CmdResult(
            True,
            r.message + " (staged in psxrecomp)",
            detail.strip(),
        )
    if stage and dry_run:
        return CmdResult(
            True,
            r.message + " (would stage in psxrecomp)",
            r.detail,
        )
    return r


def commit_nested(
    root: Path,
    message: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    """Commit inside the psxrecomp checkout (nested gitlink bumps)."""
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    r = commit_all(psx, message, dry_run=dry_run)
    if r.ok:
        r = CmdResult(r.ok, f"psxrecomp: {r.message}", r.detail)
    return r


def set_nested_branch(
    root: Path,
    path: str,
    branch: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    path = path.strip().replace("\\", "/")
    if path.startswith("psxrecomp/"):
        path = path[len("psxrecomp/") :]
    return set_submodule_branch(psx, path, branch, dry_run=dry_run)


def set_submodule_branch(
    root: Path,
    path: str,
    branch: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    branch = branch.strip()
    if not branch:
        return CmdResult(False, "Branch name required")
    cp = _read_gitmodules(root)
    section = _section_for_path(cp, path)
    if section is None:
        # Create section if submodule dir exists
        if not (root / path).exists():
            return CmdResult(False, f"No .gitmodules entry for {path}")
        section = f'submodule "{path}"'
        cp.add_section(section)
        cp.set(section, "path", path)
        # Try to keep existing url from git config
        code, url, _ = _git(root, "config", "-f", ".gitmodules", f"submodule.{path}.url")
        if code == 0 and url:
            cp.set(section, "url", url)
    cp.set(section, "branch", branch)
    _write_gitmodules(root, cp, dry_run=dry_run)
    if not dry_run:
        _git(root, "config", "-f", ".gitmodules", f"submodule.{path}.branch", branch)
        # Sync into local git config
        _git(root, "submodule", "sync", "--", path)
    return CmdResult(True, f"Set {path} tracking branch → {branch}")


def set_repo_branch(
    root: Path,
    branch: str,
    *,
    create: bool = False,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    branch = branch.strip()
    if not branch:
        return CmdResult(False, "Branch name required")
    code, current, _ = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    if code == 0 and current == branch:
        return CmdResult(True, f"Already on {branch}")
    args = ["checkout"]
    if create:
        args.append("-B")
    args.append(branch)
    code, out, err = _git(root, *args, dry_run=dry_run)
    if code != 0:
        return CmdResult(False, f"Could not checkout {branch}", err or out)
    return CmdResult(True, f"Checked out {branch}", out)


def list_branches(
    repo: Path,
    *,
    remotes: bool = True,
    fetch: bool = False,
) -> list[str]:
    """Return sorted local (+ remote-tracking) branch short names for a repo."""
    repo = repo.expanduser().resolve()
    if not repo.is_dir() or not _is_git_repo(repo):
        return []
    if fetch:
        _git(repo, "fetch", "--prune", "--quiet")
    names: set[str] = set()
    code, out, _ = _git(repo, "for-each-ref", "--format=%(refname:short)", "refs/heads")
    if code == 0:
        for line in out.splitlines():
            n = line.strip()
            if n and n != "HEAD":
                names.add(n)
    if remotes:
        code, out, _ = _git(
            repo, "for-each-ref", "--format=%(refname:short)", "refs/remotes"
        )
        if code == 0:
            for line in out.splitlines():
                n = line.strip()
                if not n or n.endswith("/HEAD") or "->" in n:
                    continue
                # origin/main → main
                if "/" in n:
                    n = n.split("/", 1)[1]
                if n and n != "HEAD":
                    names.add(n)
    return _sort_branch_names(names)


def list_remote_head_branches(url: str) -> list[str]:
    """``git ls-remote --heads`` when a local checkout is unavailable."""
    url = (url or "").strip()
    if not url:
        return []
    code, out, _ = _run(["git", "ls-remote", "--heads", url], Path.cwd())
    if code != 0:
        return []
    names: set[str] = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        ref = parts[1]
        if ref.startswith("refs/heads/"):
            names.add(ref[len("refs/heads/") :])
    return _sort_branch_names(names)


def list_module_branches(
    root: Path,
    path: str,
    *,
    nested: bool = False,
    remotes: bool = True,
    fetch: bool = False,
    url_fallback: str = "",
) -> list[str]:
    """Branches for a game submodule or a nested module inside psxrecomp."""
    root = root.expanduser().resolve()
    path = path.strip().replace("\\", "/")
    if nested:
        psx = resolve_psxrecomp_dir(root)
        if psx is None:
            return list_remote_head_branches(url_fallback) if url_fallback else []
        if path.startswith("psxrecomp/"):
            path = path[len("psxrecomp/") :]
        owner = psx
    else:
        owner = root
    sub = owner / path
    if sub.is_dir() and _is_git_repo(sub):
        return list_branches(sub, remotes=remotes, fetch=fetch)
    # Fall back to URL from .gitmodules or caller
    url = url_fallback
    if not url:
        cp = _read_gitmodules(owner)
        section = _section_for_path(cp, path)
        if section:
            url = cp.get(section, "url", fallback="")
    return list_remote_head_branches(url)


def _sort_branch_names(names: set[str] | list[str]) -> list[str]:
    preferred = ("main", "master", "develop", "development")

    def key(s: str) -> tuple:
        s_l = s.lower()
        try:
            rank = preferred.index(s_l)
        except ValueError:
            rank = len(preferred)
        return (rank, s_l)

    return sorted(set(names), key=key)


def update_submodules(
    root: Path,
    *,
    paths: list[str] | None = None,
    remote: bool = False,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    if not _is_git_repo(root):
        return CmdResult(False, "Not a git repository")
    cmd = ["submodule", "update", "--init", "--recursive"]
    if remote:
        cmd.append("--remote")
    if paths:
        cmd.append("--")
        cmd.extend(paths)
    code, out, err = _git(root, *cmd, dry_run=dry_run)
    if code != 0:
        return CmdResult(False, "Submodule update failed", err or out)
    mode = "remote tracking tip" if remote else "pinned gitlink"
    return CmdResult(True, f"Updated submodules ({mode})", out)


def pull(root: Path, *, dry_run: bool = False) -> CmdResult:
    root = root.expanduser().resolve()
    code, out, err = _git(root, "pull", "--ff-only", dry_run=dry_run)
    if code != 0:
        return CmdResult(False, "Pull failed (ff-only)", err or out)
    return CmdResult(True, "Pulled (ff-only)", out)


def commit_all(
    root: Path,
    message: str,
    *,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    message = message.strip()
    if not message:
        return CmdResult(False, "Commit message required")
    code, porcelain, _ = _git(root, "status", "--porcelain")
    if code != 0:
        return CmdResult(False, "git status failed", porcelain)
    if not porcelain.strip() and not dry_run:
        return CmdResult(False, "Nothing to commit")
    code, out, err = _git(root, "add", "-A", dry_run=dry_run)
    if code != 0:
        return CmdResult(False, "git add failed", err or out)
    code, out, err = _git(root, "commit", "-m", message, dry_run=dry_run)
    if code != 0:
        return CmdResult(False, "git commit failed", err or out)
    return CmdResult(True, "Committed", out)


def push(
    root: Path,
    *,
    branch: str = "",
    dry_run: bool = False,
) -> CmdResult:
    """Push current HEAD to origin.

    If HEAD is detached, ``branch`` is required and we push
    ``HEAD:refs/heads/<branch>`` so GitHub gets a real branch ref.
    """
    root = root.expanduser().resolve()
    code, _, err = _git(root, "remote", "get-url", "origin")
    if code != 0 and not dry_run:
        return CmdResult(False, "No origin remote", err)

    local = current_branch(root)
    target = (branch or "").strip()
    if target.startswith("("):
        target = ""

    if local:
        # On a branch: normal upstream push
        code, out, err = _git(root, "push", "-u", "origin", "HEAD", dry_run=dry_run)
        if code != 0:
            return CmdResult(False, "Push failed", err or out)
        return CmdResult(True, f"Pushed {local} → origin", out)

    # Detached HEAD
    if not target:
        return CmdResult(
            False,
            "Detached HEAD — select/checkout a branch before push "
            "(or Studio will push HEAD:refs/heads/<branch> if one is selected)",
        )
    refspec = f"HEAD:refs/heads/{target}"
    code, out, err = _git(root, "push", "-u", "origin", refspec, dry_run=dry_run)
    if code != 0:
        return CmdResult(False, f"Push failed (detached → {target})", err or out)
    # Best-effort: attach local checkout to that branch so later pushes are normal
    if not dry_run:
        _git(root, "switch", "-C", target)
        return CmdResult(
            True,
            f"Pushed detached HEAD → origin/{target} (checked out {target})",
            out,
        )
    return CmdResult(
        True,
        f"Would push detached HEAD → origin/{target} and checkout {target}",
        out,
    )


def _normalize_module_path(path: str, *, nested: bool) -> str:
    p = path.strip().replace("\\", "/")
    if nested and p.startswith("psxrecomp/"):
        p = p[len("psxrecomp/") :]
    return p


def resolve_module_dir(
    root: Path,
    path: str,
    *,
    nested: bool = False,
) -> Path | None:
    """Resolve a game submodule or a nested module checkout under psxrecomp."""
    root = root.expanduser().resolve()
    path = _normalize_module_path(path, nested=nested)
    if not path:
        return None
    if nested:
        psx = resolve_psxrecomp_dir(root)
        if psx is None:
            return None
        # Engine repo itself: nested paths are direct.
        owner = psx
    else:
        owner = root
    sub = owner / path
    if sub.is_dir() and _is_git_repo(sub):
        return sub
    return None


def default_module_paths(*, nested: bool = False) -> tuple[str, ...]:
    return NESTED_PATHS if nested else KNOWN_SUBMODULES


def pull_modules(
    root: Path,
    *,
    paths: list[str] | None = None,
    nested: bool = False,
    dry_run: bool = False,
) -> list[CmdResult]:
    """``git pull --ff-only`` inside each module checkout."""
    want = paths or list(default_module_paths(nested=nested))
    results: list[CmdResult] = []
    for path in want:
        path = _normalize_module_path(path, nested=nested)
        sub = resolve_module_dir(root, path, nested=nested)
        if sub is None:
            results.append(CmdResult(False, f"{path}: checkout missing"))
            continue
        r = pull(sub, dry_run=dry_run)
        results.append(CmdResult(r.ok, f"{path}: {r.message}", r.detail))
    return results


def push_modules(
    root: Path,
    *,
    paths: list[str] | None = None,
    nested: bool = False,
    branch_by_path: dict[str, str] | None = None,
    dry_run: bool = False,
) -> list[CmdResult]:
    """Push each module checkout to origin (handles detached HEAD via branch map)."""
    want = paths or list(default_module_paths(nested=nested))
    branches = branch_by_path or {}
    results: list[CmdResult] = []
    for path in want:
        path = _normalize_module_path(path, nested=nested)
        sub = resolve_module_dir(root, path, nested=nested)
        if sub is None:
            results.append(CmdResult(False, f"{path}: checkout missing"))
            continue
        r = push(sub, branch=branches.get(path, ""), dry_run=dry_run)
        results.append(CmdResult(r.ok, f"{path}: {r.message}", r.detail))
    return results


def commit_modules(
    root: Path,
    message: str,
    *,
    paths: list[str] | None = None,
    nested: bool = False,
    dry_run: bool = False,
) -> list[CmdResult]:
    """``git add -A && git commit`` inside each module checkout."""
    message = message.strip()
    if not message:
        return [CmdResult(False, "Commit message required")]
    want = paths or list(default_module_paths(nested=nested))
    results: list[CmdResult] = []
    for path in want:
        path = _normalize_module_path(path, nested=nested)
        sub = resolve_module_dir(root, path, nested=nested)
        if sub is None:
            results.append(CmdResult(False, f"{path}: checkout missing"))
            continue
        r = commit_all(sub, message, dry_run=dry_run)
        results.append(CmdResult(r.ok, f"{path}: {r.message}", r.detail))
    return results


def pull_psxrecomp(root: Path, *, dry_run: bool = False) -> CmdResult:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    r = pull(psx, dry_run=dry_run)
    return CmdResult(r.ok, f"psxrecomp: {r.message}", r.detail)


def push_psxrecomp(
    root: Path,
    *,
    branch: str = "",
    dry_run: bool = False,
) -> CmdResult:
    psx = resolve_psxrecomp_dir(root)
    if psx is None:
        return CmdResult(False, "No psxrecomp checkout found")
    r = push(psx, branch=branch, dry_run=dry_run)
    return CmdResult(r.ok, f"psxrecomp: {r.message}", r.detail)


def release_workflow_name(root: Path) -> str | None:
    """Return registered Actions workflow name for release.yml, if any."""
    if not _which_gh():
        return None
    code, out, _ = _run(
        [
            "gh",
            "api",
            "repos/{owner}/{repo}/actions/workflows",
            "--jq",
            '.workflows[] | select(.path|endswith("release.yml")) | .name',
        ],
        root,
    )
    if code != 0:
        return None
    for line in out.splitlines():
        name = line.strip()
        if name:
            return name
    return None


def run_release_workflow(
    root: Path,
    *,
    version: str = "",
    bump: str = "patch",
    publish: bool = True,
    reuse_cached_emitters: bool = True,
    dry_run: bool = False,
) -> CmdResult:
    root = root.expanduser().resolve()
    if not _which_gh():
        return CmdResult(False, "gh CLI not found — install GitHub CLI and auth login")
    if bump not in ("patch", "minor", "major"):
        return CmdResult(False, f"Invalid bump: {bump}")

    wf = root / ".github" / "workflows" / "release.yml"
    if not wf.is_file() and not dry_run:
        return CmdResult(False, "Missing .github/workflows/release.yml")

    # Prefer workflow file path; gh accepts it.
    cmd = [
        "gh",
        "workflow",
        "run",
        "release.yml",
        "-f",
        f"version={version}",
        "-f",
        f"bump={bump}",
        "-f",
        f"publish={'true' if publish else 'false'}",
        "-f",
        f"reuse_cached_emitters={'true' if reuse_cached_emitters else 'false'}",
    ]
    if dry_run:
        return CmdResult(True, "dry-run: " + " ".join(cmd))

    code, out, err = _run(cmd, root)
    if code != 0:
        return CmdResult(False, "Failed to dispatch release workflow", err or out)

    # Best-effort: fetch latest run URL
    run_url = ""
    code2, runs_json, _ = _run(
        [
            "gh",
            "run",
            "list",
            "--workflow",
            "release.yml",
            "--limit",
            "1",
            "--json",
            "url,databaseId,status",
        ],
        root,
    )
    if code2 == 0 and runs_json:
        try:
            runs = json.loads(runs_json)
            if runs:
                run_url = runs[0].get("url") or ""
        except json.JSONDecodeError:
            pass

    msg = "Dispatched Release builds workflow"
    if run_url:
        msg += f"\n{run_url}"
    return CmdResult(True, msg, out)
