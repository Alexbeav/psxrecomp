"""Resolve / download / unpack portable cmake-clang-v1 toolchain packs.

Shared cache matches RetComM:

  Windows: %LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/<tag>/
  Linux/macOS: $XDG_DATA_HOME/retcomm/toolchains/… or ~/.local/share/retcomm/…

Legacy %LOCALAPPDATA%/psxrecomp/… is still searched and migrated on ensure.
"""

from __future__ import annotations

import json
import os
import platform
import re
import shutil
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Optional

DEFAULT_REPO = "TechnicallyComputers/retcomm-toolchains"
PACK_ID = "cmake-clang-v1"

_ASSET = {
    "linux-x64": "cmake-clang-v1-linux-x64.zip",
    "windows-x64": "cmake-clang-v1-windows-x64.zip",
    "macos-arm64": "cmake-clang-v1-macos-universal.zip",
    "macos-x64": "cmake-clang-v1-macos-universal.zip",
}


def sys_platform_is_windows() -> bool:
    return sys.platform == "win32"


def host_artifact() -> str:
    if sys_platform_is_windows():
        return "windows-x64"
    if sys.platform == "darwin":
        return "macos-arm64" if platform.machine().lower() in ("arm64", "aarch64") else "macos-x64"
    return "linux-x64"


def cmake_name() -> str:
    return "cmake.exe" if sys_platform_is_windows() else "cmake"


def bin_looks_usable(bin_dir: Path) -> bool:
    return bin_dir.is_dir() and (bin_dir / cmake_name()).is_file()


def pack_root_looks_usable(root: Path) -> bool:
    return bin_looks_usable(root / "bin")


def unwrap_pack_root(path: Path) -> Path:
    """If *path* is a single nested directory with bin/, return that child."""
    if pack_root_looks_usable(path):
        return path
    try:
        kids = [p for p in path.iterdir() if p.is_dir() and not p.name.startswith(".")]
    except OSError:
        return path
    if len(kids) == 1 and pack_root_looks_usable(kids[0]):
        return kids[0]
    return path


def resolve_embedded_bin(project_root: Path) -> Optional[Path]:
    root = project_root / "toolchain"
    if not root.is_dir():
        return None
    direct = root / "bin"
    if bin_looks_usable(direct):
        return direct
    try:
        kids = [p for p in root.iterdir() if p.is_dir()]
    except OSError:
        return None
    if len(kids) == 1:
        nested = kids[0] / "bin"
        if bin_looks_usable(nested):
            return nested
    for kid in kids:
        nested = kid / "bin"
        if bin_looks_usable(nested):
            return nested
    return None


def env_toolchain_roots() -> list[Path]:
    out: list[Path] = []
    for key in ("RETCOMM_TOOLCHAIN_DIR", "PSXRECOMP_TOOLCHAIN_DIR", "TOOLCHAIN_DIR",
                "BPE_TOOLCHAIN_DIR"):
        raw = os.environ.get(key)
        if raw:
            out.append(Path(raw).expanduser())
    return out


def shared_cache_roots() -> list[Path]:
    """Candidate parent dirs that contain <tag>/ packs (or a flat pack).

    RetComM (`retcomm`) is preferred; legacy `psxrecomp` remains a read/migrate
    fallback.
    """
    roots: list[Path] = []
    if sys_platform_is_windows():
        local = os.environ.get("LOCALAPPDATA")
        if local:
            roots.append(Path(local) / "retcomm" / "toolchains" / PACK_ID)
            roots.append(Path(local) / "psxrecomp" / "toolchains" / PACK_ID)
    xdg = os.environ.get("XDG_DATA_HOME")
    home = Path.home()
    if xdg:
        roots.append(Path(xdg) / "retcomm" / "toolchains" / PACK_ID)
        roots.append(Path(xdg) / "psxrecomp" / "toolchains" / PACK_ID)
    else:
        roots.append(home / ".local" / "share" / "retcomm" / "toolchains" / PACK_ID)
        roots.append(home / ".local" / "share" / "psxrecomp" / "toolchains" / PACK_ID)
    # Dedup while preserving order.
    seen: set[str] = set()
    uniq: list[Path] = []
    for r in roots:
        key = str(r)
        if key not in seen:
            seen.add(key)
            uniq.append(r)
    return uniq


def preferred_install_root() -> Path:
    """Where newly downloaded / offline-unpacked packs land (RetComM shared)."""
    for r in shared_cache_roots():
        if "retcomm" in r.parts:
            r.mkdir(parents=True, exist_ok=True)
            return r
    r = shared_cache_roots()[0]
    r.mkdir(parents=True, exist_ok=True)
    return r


def read_pack_version(root: Path) -> str:
    meta = unwrap_pack_root(root) / "retcomm-toolchain.json"
    try:
        data = json.loads(meta.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, TypeError):
        return ""
    ver = data.get("version") if isinstance(data, dict) else None
    return str(ver).strip() if ver else ""


def parse_version_tuple(ver: str) -> tuple[int, ...]:
    s = ver.strip()
    if s.lower().startswith("v") and len(s) > 1 and s[1].isdigit():
        s = s[1:]
    parts: list[int] = []
    for chunk in re.split(r"[^\d]+", s):
        if not chunk:
            continue
        try:
            parts.append(int(chunk))
        except ValueError:
            break
        if len(parts) >= 4:
            break
    return tuple(parts) if parts else (0,)


def version_satisfies(have: str, need: str) -> bool:
    if not need:
        return True
    if not have:
        return False
    return parse_version_tuple(have) >= parse_version_tuple(need)


def default_min_version() -> str:
    env = (os.environ.get("RETCOMM_TOOLCHAIN_MIN_VERSION") or "").strip()
    if env:
        return env
    # Windows packs from 1.0.3 ship static zlib for find_package(ZLIB).
    if sys_platform_is_windows():
        return "1.0.3"
    return ""


def pack_satisfies_min(root: Path, min_version: str = "") -> bool:
    need = min_version or default_min_version()
    if not need:
        return True
    return version_satisfies(read_pack_version(root), need)


def _best_pack_under(base: Path, min_version: str = "") -> Optional[Path]:
    if not base.is_dir():
        return None
    if pack_root_looks_usable(base) and pack_satisfies_min(base, min_version):
        return unwrap_pack_root(base)
    prefer_names = ("latest", "offline")
    candidates: list[Path] = []
    try:
        kids = [p for p in base.iterdir() if p.is_dir() and not p.name.startswith(".")]
    except OSError:
        return None
    for name in prefer_names:
        for kid in kids:
            if kid.name == name:
                root = unwrap_pack_root(kid)
                if pack_root_looks_usable(root) and pack_satisfies_min(root, min_version):
                    return root
    for kid in kids:
        root = unwrap_pack_root(kid)
        if pack_root_looks_usable(root) and pack_satisfies_min(root, min_version):
            candidates.append(root)
    if not candidates:
        return None

    def sort_key(p: Path) -> tuple:
        ver = read_pack_version(p)
        return (parse_version_tuple(ver), p.stat().st_mtime)

    candidates.sort(key=sort_key, reverse=True)
    return candidates[0]


def migrate_legacy_psxrecomp_cache(log=None) -> None:
    """Copy legacy psxrecomp cache into retcomm when retcomm has no usable pack."""
    retcomm = next((r for r in shared_cache_roots() if "retcomm" in r.parts), None)
    legacy = next((r for r in shared_cache_roots() if "psxrecomp" in r.parts), None)
    if retcomm is None or legacy is None or not legacy.is_dir():
        return
    if _best_pack_under(retcomm):
        return
    src = _best_pack_under(legacy)
    if src is None:
        return
    tag = src.name if src.parent == legacy else "latest"
    if src == unwrap_pack_root(legacy):
        tag = "latest"
    dest = retcomm / tag
    try:
        retcomm.mkdir(parents=True, exist_ok=True)
        if dest.exists():
            shutil.rmtree(dest, ignore_errors=True)
        shutil.copytree(src, dest)
        if log:
            log(f"Migrated toolchain cache {src} -> {dest}")
    except OSError as exc:
        if log:
            log(f"Could not migrate legacy toolchain cache: {exc}")


STAMP_NAME = ".psxrecomp-bin"


def is_windows_store_python() -> bool:
    """Microsoft Store Python redirects %LOCALAPPDATA% writes into LocalCache."""
    if not sys_platform_is_windows():
        return False
    try:
        exe = str(Path(sys.executable).resolve()).lower()
    except OSError:
        exe = str(sys.executable).lower()
    return "windowsapps" in exe or "pythonsoftwarefoundation" in exe


def write_toolchain_stamp(project_root: Path, bin_dir: Path) -> None:
    """Write project_root/toolchain/.psxrecomp-bin for the C host to read."""
    stamp_dir = project_root / "toolchain"
    try:
        stamp_dir.mkdir(parents=True, exist_ok=True)
        (stamp_dir / STAMP_NAME).write_text(
            str(bin_dir.resolve()) + "\n", encoding="utf-8"
        )
    except OSError:
        pass


def materialize_into_project(
    project_root: Path, pack_root: Path, log=None
) -> Path:
    """Point the project at a shared pack via stamp (no multi‑GB copy)."""
    src = unwrap_pack_root(pack_root)
    if not pack_root_looks_usable(src):
        raise RuntimeError(f"toolchain pack unusable: {pack_root}")
    write_toolchain_stamp(project_root, src / "bin")
    if log:
        log(f"Using shared toolchain at {src}")
    return src


def find_cached_pack(min_version: str = "") -> Optional[Path]:
    for base in shared_cache_roots():
        found = _best_pack_under(base, min_version=min_version)
        if found is not None:
            return found
    return None


def resolve_toolchain_bin(
    project_root: Optional[Path] = None, *, min_version: str = ""
) -> Optional[Path]:
    for env_root in env_toolchain_roots():
        root = unwrap_pack_root(env_root)
        if pack_root_looks_usable(root) and pack_satisfies_min(root, min_version):
            return root / "bin"
    if project_root is not None:
        embedded = resolve_embedded_bin(project_root)
        if embedded:
            root = embedded.parent
            if pack_satisfies_min(root, min_version):
                return embedded
    cached = find_cached_pack(min_version=min_version)
    if cached:
        return cached / "bin"
    return None


def activate_toolchain_bin(bin_dir: Path, log=None) -> None:
    prefix = str(bin_dir)
    pack_root = bin_dir.parent
    cur = os.environ.get("PATH", "")
    parts = cur.split(os.pathsep) if cur else []
    if not parts or Path(parts[0]) != bin_dir:
        os.environ["PATH"] = prefix + (os.pathsep + cur if cur else "")
    # Windows cmake-clang-v1 ships zlib under the pack root; help FindZLIB.
    pack_s = str(pack_root)
    os.environ["ZLIB_ROOT"] = pack_s
    os.environ["RETCOMM_TOOLCHAIN_DIR"] = pack_s
    if "PSXRECOMP_TOOLCHAIN_DIR" not in os.environ:
        os.environ["PSXRECOMP_TOOLCHAIN_DIR"] = pack_s
    prev_prefix = os.environ.get("CMAKE_PREFIX_PATH", "")
    if not prev_prefix:
        os.environ["CMAKE_PREFIX_PATH"] = pack_s
    elif pack_s not in prev_prefix.split(os.pathsep):
        os.environ["CMAKE_PREFIX_PATH"] = pack_s + os.pathsep + prev_prefix
    for name, env_key in (
        ("clang", "CC"),
        ("clang++", "CXX"),
        ("clang.exe", "CC"),
        ("clang++.exe", "CXX"),
    ):
        cand = bin_dir / name
        if cand.is_file() and env_key not in os.environ:
            os.environ[env_key] = str(cand)
    if log:
        log(f"Using toolchain: {bin_dir}")


def unpack_zip_to(zip_path: Path, dest: Path) -> Path:
    """Extract *zip_path* into *dest* (replaced) and return usable pack root."""
    if dest.exists():
        shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(dest)
    root = unwrap_pack_root(dest)
    if not pack_root_looks_usable(root):
        raise RuntimeError(f"toolchain zip missing bin/{cmake_name()}: {zip_path}")
    # If unwrap pointed at a child, normalize to dest by moving contents up when
    # dest itself is not the pack root.
    if root != dest and pack_root_looks_usable(root):
        # Leave nested layout — resolve/activate handle it via unwrap.
        return root
    return root


def _install_tag_for_root(root: Path, fallback: str) -> str:
    ver = read_pack_version(root)
    if ver:
        safe = re.sub(r"[^\w.\-]+", "_", ver.strip())
        return safe or fallback
    return fallback


def install_from_zip(
    zip_path: Path,
    tag: str = "offline",
    *,
    project_root: Optional[Path] = None,
    min_version: str = "",
    log=None,
) -> Path:
    zip_path = zip_path.expanduser().resolve()
    if not zip_path.is_file():
        raise FileNotFoundError(f"toolchain zip not found: {zip_path}")
    staging = preferred_install_root() / ".staging-offline"
    root = unpack_zip_to(zip_path, staging)
    if not pack_satisfies_min(root, min_version):
        need = min_version or default_min_version()
        have = read_pack_version(root) or "(unknown)"
        raise RuntimeError(
            f"Toolchain zip version {have} does not meet min_version {need}."
        )
    dest_tag = _install_tag_for_root(root, tag)
    dest = preferred_install_root() / dest_tag
    if dest.resolve() != root.resolve():
        if dest.exists():
            shutil.rmtree(dest, ignore_errors=True)
        shutil.move(str(root), str(dest))
        root = unwrap_pack_root(dest)
        # Drop empty staging parent left behind by a nested unzip layout.
        if staging.exists() and staging != dest:
            shutil.rmtree(staging, ignore_errors=True)
    # Keep a `latest` pointer directory for simple resolvers.
    latest = preferred_install_root() / "latest"
    if latest.resolve() != root.resolve():
        if latest.exists() or latest.is_symlink():
            if latest.is_dir() and not latest.is_symlink():
                shutil.rmtree(latest, ignore_errors=True)
            else:
                latest.unlink(missing_ok=True)
        try:
            latest.symlink_to(root, target_is_directory=True)
        except OSError:
            shutil.copytree(root, latest)
    if project_root is not None:
        return materialize_into_project(project_root, root, log=log)
    return root


def download_url(url: str, dest: Path, token: Optional[str] = None) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "psxrecomp-toolchain"})
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=120) as resp, open(dest, "wb") as out:
        shutil.copyfileobj(resp, out)


def download_latest_pack(
    artifact: Optional[str] = None,
    repo: str = DEFAULT_REPO,
    log=None,
    *,
    project_root: Optional[Path] = None,
    min_version: str = "",
) -> Path:
    art = artifact or host_artifact()
    asset = _ASSET.get(art)
    if not asset:
        raise RuntimeError(f"unknown toolchain artifact: {art}")
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
    url = f"https://github.com/{repo}/releases/latest/download/{asset}"
    if log:
        log(f"Downloading {asset} from {repo}...")
    with tempfile.TemporaryDirectory(prefix="psxrecomp-tc-") as tmp:
        zpath = Path(tmp) / asset
        try:
            download_url(url, zpath, token=token or None)
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"toolchain download failed ({e.code}): {url}") from e
        except urllib.error.URLError as e:
            raise RuntimeError(f"toolchain download failed: {e.reason}") from e
        return install_from_zip(
            zpath,
            tag="latest",
            project_root=project_root,
            min_version=min_version,
            log=log,
        )


def ensure_toolchain(
    project_root: Optional[Path] = None,
    *,
    from_zip: Optional[Path] = None,
    download: bool = False,
    repo: str = DEFAULT_REPO,
    min_version: str = "",
    log=None,
) -> Path:
    """Return usable toolchain *bin* directory, installing if requested.

    Resolution order: env override → project stamp/toolchain/ → shared
    retcomm cache (legacy psxrecomp migrated) → optional --from-zip → download.

    Installs always land under the shared RetComM cache; the project only gets
    a stamp file pointing at that pack.
    """
    need = min_version or default_min_version()
    if is_windows_store_python() and log:
        log(
            "Warning: Microsoft Store Python redirects AppData writes. "
            "Prefer python.org Python if toolchain setup fails to find cmake."
        )

    migrate_legacy_psxrecomp_cache(log=log)

    if from_zip is not None:
        root = install_from_zip(
            Path(from_zip),
            project_root=project_root,
            min_version=need,
            log=log,
        )
        bin_dir = unwrap_pack_root(root) / "bin"
        if not bin_looks_usable(bin_dir):
            bin_dir = root / "bin"
        if project_root is not None:
            write_toolchain_stamp(project_root, bin_dir)
        activate_toolchain_bin(bin_dir, log=log)
        return bin_dir

    existing = resolve_toolchain_bin(project_root, min_version=need)
    if existing:
        if project_root is not None:
            write_toolchain_stamp(project_root, existing)
        activate_toolchain_bin(existing, log=log)
        return existing

    # Usable pack exists but is too old — fall through to download/replace.
    stale = resolve_toolchain_bin(project_root, min_version="")
    if stale and need and log:
        log(
            f"Cached toolchain does not meet min_version {need}; "
            "will download/replace."
        )

    if download:
        root = download_latest_pack(
            repo=repo, log=log, project_root=project_root, min_version=need
        )
        bin_dir = unwrap_pack_root(root) / "bin"
        if not bin_looks_usable(bin_dir):
            bin_dir = root / "bin"
        if project_root is not None:
            write_toolchain_stamp(project_root, bin_dir)
        activate_toolchain_bin(bin_dir, log=log)
        return bin_dir

    raise RuntimeError(
        "No portable toolchain found. Pass --from-zip PATH to a "
        "cmake-clang-v1-*.zip, use --download, set RETCOMM_TOOLCHAIN_DIR / "
        "PSXRECOMP_TOOLCHAIN_DIR, or install cmake on PATH."
        + (f" (required min_version {need})" if need else "")
    )
