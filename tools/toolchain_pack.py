"""Resolve / download / unpack portable cmake-clang-v1 toolchain packs.

Used by psxrecomp_cli (ensure-toolchain, rebuild) and mirrors RetComM's
shared-cache layout when possible.
"""

from __future__ import annotations

import os
import platform
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
    for key in ("PSXRECOMP_TOOLCHAIN_DIR", "RETCOMM_TOOLCHAIN_DIR", "TOOLCHAIN_DIR",
                "BPE_TOOLCHAIN_DIR"):
        raw = os.environ.get(key)
        if raw:
            out.append(Path(raw).expanduser())
    return out


def shared_cache_roots() -> list[Path]:
    """Candidate parent dirs that contain <tag>/ packs (or a flat pack)."""
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
    """Where newly downloaded / offline-unpacked packs land."""
    for r in shared_cache_roots():
        # Prefer psxrecomp-owned cache for writes; fall through to first.
        if "psxrecomp" in r.parts:
            r.mkdir(parents=True, exist_ok=True)
            return r
    r = shared_cache_roots()[0]
    r.mkdir(parents=True, exist_ok=True)
    return r


def find_cached_pack() -> Optional[Path]:
    for base in shared_cache_roots():
        if not base.is_dir():
            continue
        if pack_root_looks_usable(base):
            return unwrap_pack_root(base)
        try:
            kids = sorted(
                [p for p in base.iterdir() if p.is_dir()],
                key=lambda p: p.stat().st_mtime,
                reverse=True,
            )
        except OSError:
            continue
        for kid in kids:
            root = unwrap_pack_root(kid)
            if pack_root_looks_usable(root):
                return root
    return None


def resolve_toolchain_bin(project_root: Optional[Path] = None) -> Optional[Path]:
    for env_root in env_toolchain_roots():
        root = unwrap_pack_root(env_root)
        if pack_root_looks_usable(root):
            return root / "bin"
    if project_root is not None:
        embedded = resolve_embedded_bin(project_root)
        if embedded:
            return embedded
    cached = find_cached_pack()
    if cached:
        return cached / "bin"
    return None


def activate_toolchain_bin(bin_dir: Path, log=None) -> None:
    prefix = str(bin_dir)
    cur = os.environ.get("PATH", "")
    parts = cur.split(os.pathsep) if cur else []
    if not parts or Path(parts[0]) != bin_dir:
        os.environ["PATH"] = prefix + (os.pathsep + cur if cur else "")
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


def install_from_zip(zip_path: Path, tag: str = "offline") -> Path:
    zip_path = zip_path.expanduser().resolve()
    if not zip_path.is_file():
        raise FileNotFoundError(f"toolchain zip not found: {zip_path}")
    dest = preferred_install_root() / tag
    return unpack_zip_to(zip_path, dest)


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
) -> Path:
    art = artifact or host_artifact()
    asset = _ASSET.get(art)
    if not asset:
        raise RuntimeError(f"unknown toolchain artifact: {art}")
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
    url = f"https://github.com/{repo}/releases/latest/download/{asset}"
    if log:
        log(f"Downloading {asset} from {repo}…")
    with tempfile.TemporaryDirectory(prefix="psxrecomp-tc-") as tmp:
        zpath = Path(tmp) / asset
        try:
            download_url(url, zpath, token=token or None)
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"toolchain download failed ({e.code}): {url}") from e
        except urllib.error.URLError as e:
            raise RuntimeError(f"toolchain download failed: {e.reason}") from e
        dest = preferred_install_root() / "latest"
        root = unpack_zip_to(zpath, dest)
        if log:
            log(f"Installed toolchain pack at {root}")
        return root


def ensure_toolchain(
    project_root: Optional[Path] = None,
    *,
    from_zip: Optional[Path] = None,
    download: bool = False,
    repo: str = DEFAULT_REPO,
    log=None,
) -> Path:
    """Return usable toolchain *bin* directory, installing if requested.

    Resolution order: env override → project toolchain/ → shared cache →
    optional --from-zip → optional download.
    """
    if from_zip is not None:
        root = install_from_zip(Path(from_zip))
        bin_dir = root / "bin"
        activate_toolchain_bin(bin_dir, log=log)
        return bin_dir

    existing = resolve_toolchain_bin(project_root)
    if existing:
        activate_toolchain_bin(existing, log=log)
        return existing

    if download:
        root = download_latest_pack(repo=repo, log=log)
        bin_dir = unwrap_pack_root(root) / "bin"
        if not bin_looks_usable(bin_dir):
            bin_dir = root / "bin"
        activate_toolchain_bin(bin_dir, log=log)
        return bin_dir

    raise RuntimeError(
        "No portable toolchain found. Pass --from-zip PATH to a "
        "cmake-clang-v1-*.zip, use --download, set PSXRECOMP_TOOLCHAIN_DIR / "
        "RETCOMM_TOOLCHAIN_DIR, or install cmake on PATH."
    )
