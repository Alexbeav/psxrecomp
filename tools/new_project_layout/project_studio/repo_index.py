"""Persistent indexed list of local game repos for Project Studio."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path

_TOOLKIT = Path(__file__).resolve().parent.parent
DEFAULT_INDEX_PATH = _TOOLKIT / "project_studio_repos.json"


@dataclass
class RepoEntry:
    path: str
    name: str = ""
    cue: str = ""  # absolute path to Redump .cue when known

    def resolved(self) -> Path:
        return Path(self.path).expanduser().resolve()

    def display(self) -> str:
        name = (self.name or "").strip() or Path(self.path).name
        return name

    def label(self) -> str:
        """Unique-ish label for dropdowns (name · basename if needed)."""
        p = Path(self.path)
        name = self.display()
        if name != p.name:
            return f"{name}  ({p.name})"
        return name

    def cue_path(self) -> Path | None:
        raw = (self.cue or "").strip()
        if not raw:
            return None
        p = Path(raw).expanduser()
        if not p.is_absolute():
            p = self.resolved() / p
        try:
            p = p.resolve()
        except OSError:
            return p
        return p if p.is_file() else p


@dataclass
class RepoIndex:
    repos: list[RepoEntry]
    last: str = ""
    path: Path = DEFAULT_INDEX_PATH

    def to_dict(self) -> dict:
        return {
            "last": self.last,
            "repos": [asdict(r) for r in self.repos],
        }

    def find(self, root: Path | str) -> RepoEntry | None:
        try:
            key = str(Path(str(root)).expanduser().resolve())
        except OSError:
            key = str(root).strip()
        for entry in self.repos:
            if entry.path == key:
                return entry
        return None


def _toml_game_field(root: Path, field: str) -> str:
    gt = root / "game.toml"
    if not gt.is_file():
        return ""
    try:
        text = gt.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    in_game = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_game = s == "[game]"
            continue
        if not in_game or "=" not in s or s.startswith("#"):
            continue
        key, _, val = s.partition("=")
        if key.strip() != field:
            continue
        return val.strip().strip('"').strip("'")
    return ""


def _game_toml_name(root: Path) -> str:
    return _toml_game_field(root, "name") or root.name


def discover_cue(root: Path) -> str:
    """Best-effort .cue path from game.toml or common disc/ layouts."""
    root = root.expanduser().resolve()
    rel = _toml_game_field(root, "disc")
    if rel:
        cand = Path(rel)
        if not cand.is_absolute():
            cand = root / cand
        try:
            cand = cand.resolve()
        except OSError:
            pass
        if cand.is_file():
            return str(cand)
    # Fallbacks: disc/*.cue (prefer name matching game.toml cue_name)
    cue_name = ""
    gt = root / "game.toml"
    if gt.is_file():
        try:
            text = gt.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        in_prep = False
        for line in text.splitlines():
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                in_prep = s == "[prepare_disc]"
                continue
            if not in_prep or "=" not in s or s.startswith("#"):
                continue
            key, _, val = s.partition("=")
            if key.strip() == "cue_name":
                cue_name = val.strip().strip('"').strip("'")
                break
    disc_dir = root / "disc"
    if disc_dir.is_dir():
        if cue_name:
            named = disc_dir / cue_name
            if named.is_file():
                return str(named.resolve())
        cues = sorted(disc_dir.glob("*.cue"))
        if len(cues) == 1:
            return str(cues[0].resolve())
        if cues and cue_name:
            for c in cues:
                if c.name == cue_name:
                    return str(c.resolve())
    return ""


def looks_like_game_repo(root: Path) -> bool:
    root = root.expanduser().resolve()
    if not root.is_dir():
        return False
    if (root / "game.toml").is_file():
        return True
    if (root / "CMakeLists.txt").is_file() and (
        (root / "psxrecomp").exists() or (root / "runtime").exists()
    ):
        return True
    return False


def load_index(path: Path | None = None) -> RepoIndex:
    path = path or DEFAULT_INDEX_PATH
    if not path.is_file():
        return RepoIndex(repos=[], last="", path=path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return RepoIndex(repos=[], last="", path=path)
    repos: list[RepoEntry] = []
    seen: set[str] = set()
    for raw in data.get("repos") or []:
        if isinstance(raw, str):
            p = raw
            name = ""
            cue = ""
        elif isinstance(raw, dict):
            p = str(raw.get("path") or "").strip()
            name = str(raw.get("name") or "").strip()
            cue = str(raw.get("cue") or raw.get("disc") or "").strip()
        else:
            continue
        if not p:
            continue
        try:
            key = str(Path(p).expanduser().resolve())
        except OSError:
            key = p
        if key in seen:
            continue
        seen.add(key)
        root_p = Path(key)
        if not name:
            try:
                name = _game_toml_name(root_p) if root_p.is_dir() else Path(p).name
            except OSError:
                name = Path(p).name
        if cue:
            try:
                cue_p = Path(cue).expanduser()
                if not cue_p.is_absolute() and root_p.is_dir():
                    cue_p = root_p / cue_p
                cue = str(cue_p.resolve())
            except OSError:
                pass
        elif root_p.is_dir():
            cue = discover_cue(root_p)
        repos.append(RepoEntry(path=key, name=name, cue=cue))
    last = str(data.get("last") or "").strip()
    if last:
        try:
            last = str(Path(last).expanduser().resolve())
        except OSError:
            pass
    return RepoIndex(repos=repos, last=last, path=path)


def save_index(index: RepoIndex) -> None:
    path = index.path or DEFAULT_INDEX_PATH
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(index.to_dict(), indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )


def add_repo(
    index: RepoIndex,
    root: Path,
    *,
    name: str = "",
    cue: str = "",
) -> RepoEntry:
    root = root.expanduser().resolve()
    key = str(root)
    cue_s = ""
    if cue:
        try:
            cue_s = str(Path(cue).expanduser().resolve())
        except OSError:
            cue_s = str(cue).strip()
    else:
        cue_s = discover_cue(root)
    for existing in index.repos:
        if existing.path == key:
            if name and name != existing.name:
                existing.name = name
            if cue_s and not existing.cue:
                existing.cue = cue_s
            elif cue and cue_s:
                existing.cue = cue_s
            index.last = key
            save_index(index)
            return existing
    entry = RepoEntry(
        path=key,
        name=(name or _game_toml_name(root)),
        cue=cue_s,
    )
    index.repos.append(entry)
    index.repos.sort(key=lambda e: e.display().lower())
    index.last = key
    save_index(index)
    return entry


def set_repo_cue(index: RepoIndex, root: Path | str, cue: Path | str) -> RepoEntry | None:
    """Store / update the .cue path for an indexed repo."""
    entry = index.find(root)
    if entry is None:
        return None
    cue_p = Path(str(cue)).expanduser()
    try:
        cue_p = cue_p.resolve()
    except OSError:
        pass
    entry.cue = str(cue_p)
    save_index(index)
    return entry


def clear_repo_cue(index: RepoIndex, root: Path | str) -> bool:
    entry = index.find(root)
    if entry is None:
        return False
    if not entry.cue:
        return False
    entry.cue = ""
    save_index(index)
    return True


def remove_repo(index: RepoIndex, root: Path | str) -> bool:
    try:
        key = str(Path(str(root)).expanduser().resolve())
    except OSError:
        key = str(root).strip()
    before = len(index.repos)
    index.repos = [r for r in index.repos if r.path != key]
    if index.last == key:
        index.last = index.repos[0].path if index.repos else ""
    if len(index.repos) != before:
        save_index(index)
        return True
    return False


def set_last(index: RepoIndex, root: Path | str) -> None:
    try:
        key = str(Path(str(root)).expanduser().resolve())
    except OSError:
        key = str(root).strip()
    if any(r.path == key for r in index.repos):
        index.last = key
        save_index(index)


def labels_for(index: RepoIndex) -> list[str]:
    """Build unique dropdown labels; disambiguate duplicate names with parent."""
    counts: dict[str, int] = {}
    for r in index.repos:
        counts[r.display()] = counts.get(r.display(), 0) + 1
    labels: list[str] = []
    for r in index.repos:
        base = r.display()
        if counts[base] > 1:
            parent = Path(r.path).parent.name
            labels.append(f"{base}  [{parent}]")
        else:
            labels.append(base)
    seen: dict[str, int] = {}
    out: list[str] = []
    for lab in labels:
        if lab not in seen:
            seen[lab] = 0
            out.append(lab)
            continue
        seen[lab] += 1
        out.append(f"{lab}  #{seen[lab]+1}")
    return out


def path_for_label(index: RepoIndex, label: str) -> str | None:
    labs = labels_for(index)
    for lab, entry in zip(labs, index.repos):
        if lab == label:
            return entry.path
    for entry in index.repos:
        if entry.display() == label or entry.path == label:
            return entry.path
    return None


def entry_for_label(index: RepoIndex, label: str) -> RepoEntry | None:
    path = path_for_label(index, label)
    if not path:
        return None
    return index.find(path)
