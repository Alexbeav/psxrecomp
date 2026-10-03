"""Resolve / download / unpack portable cmake-clang-v1 toolchain packs.

Shared cache matches Retro / retcomm-toolchains install.sh:

  Windows: %LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/<tag>/
  Linux/macOS: $XDG_DATA_HOME/retcomm/… and ~/.local/share/retcomm/…
  plus latest → current <tag> (pointer only; never a pack extract target)

Overrides: RETCOMM_TOOLCHAIN_CACHE, RETCOMM_DATA_HOME, RETCOMM_TOOLCHAIN_DIR.
Legacy …/psxrecomp/toolchains/… is still searched and migrated on ensure.
"""

from __future__ import annotations

import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Optional

DEFAULT_REPO = "RetroPortingToolKit/RetroPorting-Toolchains"
PACK_ID = "cmake-clang-v1"

_ASSET = {
    "linux-x64": "cmake-clang-v1-linux-x64.zip",
    "windows-x64": "cmake-clang-v1-windows-x64.zip",
    "macos-arm64": "cmake-clang-v1-macos-universal.zip",
    "macos-x64": "cmake-clang-v1-macos-universal.zip",
}


# PSXRECOMP_TOOLCHAIN_READONLY=1: never delete, rename, prune or install a pack,
# never move a pointer to one (latest, the project stamp) and never write the
# user's login PATH. A pack that is there is still found and used. Tests and
# gates that start a setup program or this CLI set it: on 2026-10-02 a setup
# host started by a test installed a pack over a build host's own and emptied
# it under running builds (PS1B-406, PS1B-410). host/psxrecomp_codegen_host.c
# honours the same variable.
READONLY_ENV = "PSXRECOMP_TOOLCHAIN_READONLY"


def toolchain_readonly() -> bool:
    value = os.environ.get(READONLY_ENV, "")
    return bool(value) and not value.startswith("0")


def _readonly_skip(what: str, path=None, log=None) -> bool:
    """True under the switch, after saying what was left undone."""
    if not toolchain_readonly():
        return False
    text = f"toolchain read-only ({READONLY_ENV}): not done: {what}" + (f": {path}" if path else "")
    if log:
        log(text)
    else:
        print(text, file=sys.stderr)
    return True


# The order of every change to an installed pack (PS1B-410). A pack that is in
# use is never removed first: on 2026-10-02 an install over the installed tag
# removed that pack before the new one was in place, under running builds; the
# files that were open survived and the rest went.
#
#   - a new pack is unpacked beside the installed one and checked there;
#   - the installed folder is then renamed aside (_set_aside). A folder with an
#     open file cannot be renamed on Windows: the installed pack then stays and
#     the new one is dropped;
#   - the new pack takes the name, the pointer follows, the pack is checked
#     again in its place, and only then the folder that was set aside is
#     removed. A pack that fails in its place is taken out and the old one is
#     put back. What cannot be removed whole stays under its dot-name (no
#     lookup reads dot-names) and goes on a later pass;
#   - a pack that fails its check is reported, never removed or renamed;
#   - a folder that an interrupted install left set aside (its name is missing)
#     is put back before any check of the pointers and before any prune
#     (put_back_set_aside). A set-aside folder is removed only when its owner
#     has ended and the folder it was taken from is there again.
# host/psxrecomp_codegen_host.c follows the same order with the same names.
ASIDE_PREFIX = ".old-"


def _is_link(path: Path) -> bool:
    """True for a symlink and for a Windows junction: a pointer, not a pack."""
    try:
        if path.is_symlink():
            return True
        os.readlink(path)
        return True
    except (OSError, ValueError):
        return False


def _remove_pointer(path: Path) -> bool:
    """Remove a link itself. Never what it points at."""
    for remove in (os.unlink, os.rmdir):
        try:
            remove(path)
            return True
        except OSError:
            continue
    return not (path.exists() or _is_link(path))


# The system's error of the last rename aside that failed: winerror on Windows
# (32 a file is open in another program, 5 access denied), errno elsewhere.
_aside_error = 0


def _set_aside(path: Path) -> Optional[Path]:
    """Rename a folder to .old-<pid>[.<n>]-<name> beside it. None when it
    cannot be renamed: nothing has changed then, and _aside_error holds the
    system's error number."""
    global _aside_error
    for n in range(100):
        aside = path.with_name("%s%d%s-%s" % (ASIDE_PREFIX, os.getpid(), ".%d" % n if n else "", path.name))
        if aside.exists() or _is_link(aside):
            continue
        try:
            os.rename(path, aside)
            return aside
        except OSError as exc:
            _aside_error = int(getattr(exc, "winerror", None) or exc.errno or 0)
            return None
    return None


def _aside_parse(name: str) -> Optional[tuple]:
    """(owner's process id, the folder's own name) of ".old-<pid>[.<n>]-<name>", else None."""
    m = re.match(r"^\.old-(\d+)(?:\.\d+)?-(.+)$", name)
    return (int(m.group(1)), m.group(2)) if m else None


def _process_is_running(pid: int) -> bool:
    """Is another process with this id running? This process counts as not
    running: what it set aside and did not put back is its own to handle."""
    if pid == os.getpid():
        return False
    if sys.platform == "win32":
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, pid)      # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.GetLastError() == 5                   # there, and not ours to read
        code = ctypes.c_ulong(0)
        ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel32.CloseHandle(handle)
        return bool(ok) and code.value == 259                   # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def _aside_fate(aside: Path) -> str:
    """"leave": its owner is running (it may be in the middle of an install).
    "put back": the folder it was taken from is missing; an install was
    interrupted and the whole old pack is in it. "remove": the replacement is
    in place and nobody needs it."""
    parsed = _aside_parse(aside.name)
    if parsed is None or _process_is_running(parsed[0]):
        return "leave"
    target = aside.with_name(parsed[1])
    return "remove" if (target.exists() or _is_link(target)) else "put back"


def put_back_set_aside(log=None) -> None:
    """Put back, in every cache base, the packs an interrupted install left set
    aside. Nothing reads a dot-name, so without this the pack is lost to every
    lookup and removed by the next prune."""
    for base in shared_cache_roots():
        try:
            asides = [c for c in base.iterdir() if c.name.startswith(ASIDE_PREFIX)]
        except OSError:
            continue
        for aside in asides:
            if _aside_fate(aside) != "put back" or not pack_root_looks_usable(aside):
                continue
            if _readonly_skip("put back a pack an interrupted install left set aside", aside, log):
                continue
            target = aside.with_name(_aside_parse(aside.name)[1])
            try:
                os.rename(aside, target)
                if log:
                    log(f"An install was interrupted; the toolchain pack it had set aside is put back: {target}")
            except OSError as exc:
                if log:
                    log(f"Could not put back the toolchain pack {aside}: {exc}")


def _remove_aside(aside: Optional[Path], log=None) -> bool:
    """Remove a folder that was set aside. True when it is gone whole."""
    if aside is None:
        return True
    shutil.rmtree(aside, ignore_errors=True)
    gone = not aside.exists()
    if not gone and log:
        log(f"Could not remove the old toolchain folder {aside}; it is out of use and goes on a later pass.")
    return gone


def _remove_old_asides(cache_root: Path, log=None) -> None:
    """Set-aside folders nobody needs: the owner has ended and the folder it
    was taken from is there again. Another program's folder stays."""
    try:
        children = [c for c in cache_root.iterdir() if c.name.startswith(ASIDE_PREFIX)]
    except OSError:
        return
    for child in children:
        if _aside_fate(child) == "remove":
            _remove_aside(child, log)


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


def pack_matches_host(root: Path) -> bool:
    """True when the pack was built for this OS (retcomm-toolchain.json "os").

    Linux and macOS take a pack only when it was built for them; a Windows pack
    left in the shared cache must never reach PATH there. A pack without a
    manifest falls back to the executable-name check in bin_looks_usable.
    """
    try:
        data = json.loads((root / "retcomm-toolchain.json").read_text(encoding="utf-8"))
        pack_os = str(data.get("os", "")).lower()
    except (OSError, ValueError):
        return pack_root_looks_usable(root)
    want = host_artifact().split("-", 1)[0]
    return pack_os.startswith(want)


def toolchain_bin_runs(bin_dir: Path, log=None) -> bool:
    """True when bin/cmake exists and ``cmake --version`` exits 0."""
    if not bin_looks_usable(bin_dir):
        return False
    cmake = bin_dir / cmake_name()
    try:
        proc = subprocess.run(
            [str(cmake), "--version"],
            capture_output=True,
            text=True,
            timeout=45,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        if log:
            log(f"Toolchain cmake probe failed ({cmake}): {exc}")
        return False
    if proc.returncode != 0 and log:
        err = (proc.stderr or proc.stdout or "").strip().splitlines()
        detail = err[0] if err else f"exit {proc.returncode}"
        log(f"Toolchain cmake unusable ({cmake}): {detail}")
    return proc.returncode == 0


def toolchain_bin_check(bin_dir: Path, log=None) -> str:
    """The setup host's check of a pack (toolchain_bin_is_healthy), step by
    step. "" when the pack passes, else the step that failed:

      cmake does not run / clang or the linker is missing from the pack /
      clang does not run / a test file could not be written to the temporary
      folder / a one-line test program did not compile and link.

    A new pack is checked with this before and after it is installed, so the
    CLI and the setup host accept and refuse the same packs. (The lookup of a
    pack that is already installed keeps toolchain_bin_runs: cmake --version.)
    """
    windows = sys_platform_is_windows()
    exe = ".exe" if windows else ""
    if not toolchain_bin_runs(bin_dir, log=log):
        return "cmake does not run"
    clang = bin_dir / ("clang" + exe)
    linkers = [bin_dir / ("ld.lld" + exe)] + ([bin_dir / "lld.exe"] if windows else [])
    if not clang.is_file():
        # The macOS pack ships cmake, ninja, ccache and python only; compiling
        # uses Apple's command line tools. No clang is its normal layout.
        return "" if sys.platform == "darwin" else "clang or the linker is missing from the pack"
    if not any(l.is_file() for l in linkers):
        return "clang or the linker is missing from the pack"
    env = dict(os.environ)
    env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
    try:
        ran = subprocess.run([str(clang), "--version"], capture_output=True, timeout=60, check=False, env=env)
    except (OSError, ValueError, subprocess.SubprocessError):
        return "clang does not run"
    if ran.returncode != 0:
        return "clang does not run"
    try:
        work = Path(tempfile.mkdtemp(prefix="psxrecomp-tc-probe-"))
        (work / "probe.c").write_text("int main(void){return 0;}\n", encoding="utf-8")
    except OSError:
        return "a test file could not be written to the temporary folder"
    try:
        command = [str(clang)] + ([] if windows else ["-fuse-ld=lld"]) + [str(work / "probe.c"), "-o", str(work / ("probe" + exe))]
        try:
            built = subprocess.run(command, capture_output=True, timeout=180, check=False, env=env)
        except (OSError, ValueError, subprocess.SubprocessError):
            return "a one-line test program did not compile and link"
        if built.returncode != 0:
            if log:
                tail = (built.stderr or built.stdout or b"").decode("utf-8", errors="replace").strip().splitlines()
                log("Toolchain compile check failed: %s" % (tail[-1] if tail else "exit %d" % built.returncode))
            return "a one-line test program did not compile and link"
        return ""
    finally:
        shutil.rmtree(work, ignore_errors=True)


class ToolchainRefused(RuntimeError):
    """An install that stopped with one of the sentences a player reads. The
    exception's text is the whole sentence: a caller prints it alone, with no
    label in front and no exception name."""


def _new_pack_failed_text(step: str) -> str:
    """What a player reads when a new pack fails its check. The same sentence
    in host/psxrecomp_codegen_host.c."""
    return ("The new toolchain pack did not pass its check: %s. "
            "The installed toolchain was not changed." % step)


def _rename_failed_text(error: int) -> str:
    """What a player reads when the installed folder cannot be renamed aside.
    The same sentence in host/psxrecomp_codegen_host.c."""
    if sys_platform_is_windows() and error == 32:
        return ("Another program holds the installed toolchain folder open "
                "(Windows error 32). The toolchain was not changed. Close the "
                "programs that use it, for example a running build, and try again.")
    if sys_platform_is_windows() and error == 5:
        return ("Windows refused to rename the installed toolchain folder (error "
                "5, access denied). The toolchain was not changed. A file in it "
                "may be open in another program, or the folder may be protected.")
    return ("The installed toolchain folder could not be renamed (system error "
            "%d). The toolchain was not changed." % error)


def clear_project_toolchain_stamp(project_root: Optional[Path]) -> None:
    if project_root is None:
        return
    stamp = project_root / "toolchain" / STAMP_NAME
    try:
        if stamp.is_file():
            if _readonly_skip("remove the project's toolchain stamp", stamp):
                return
            stamp.unlink()
    except OSError:
        pass


def prune_old_toolchain_tags(keep_pack: Path, log=None) -> int:
    """Delete sibling versioned ``<tag>/`` dirs under the managed install root.

    Keeps *keep_pack* (and anything under it), ``latest/``, and dot-dirs
    (staging). Returns the number of removed entries. No-op when *keep_pack*
    is outside ``preferred_install_root()`` (e.g. ``RETCOMM_TOOLCHAIN_DIR``).
    """
    keep = unwrap_pack_root(Path(keep_pack).expanduser())
    try:
        cache_root = preferred_install_root()
    except OSError:
        return 0
    if not cache_root.is_dir():
        return 0
    if not (_paths_equal(keep, cache_root) or _is_under(keep, cache_root)):
        return 0
    if _readonly_skip("prune older toolchain installs", cache_root, log):
        return 0
    put_back_set_aside(log)     # a pack an interrupted install left aside is not pruned
    removed = 0
    try:
        children = list(cache_root.iterdir())
    except OSError:
        return 0
    for child in children:
        name = child.name
        if name.startswith(".") or name == "latest":
            continue
        try:
            if not (child.is_dir() or child.is_symlink()):
                continue
        except OSError:
            continue
        if (
            _paths_equal(keep, child)
            or _is_under(keep, child)
            or _is_under(child, keep)
        ):
            continue
        if log:
            log(f"Removing old toolchain install: {child}")
        if _is_link(child) or child.is_file():
            if _remove_pointer(child):
                removed += 1
            continue
        # Out of use first, then removed: a folder with an open file cannot be
        # renamed on Windows, and is then left whole instead of half removed.
        aside = _set_aside(child)
        if aside is None:
            if log:
                log(f"Old toolchain {child} is in use by another program; kept.")
            continue
        _remove_aside(aside, log)
        removed += 1
    _remove_old_asides(cache_root, log)
    return removed


def heal_broken_toolchain_pointers(log=None) -> None:
    """Remove ``latest`` pointers that point at nothing.

    Only a link whose target has no bin/cmake is removed, and only the link.
    A ``latest`` that is a real folder is a pack: it is never removed here,
    and neither is a pack whose cmake does not run. One failed check is not
    proof that a pack is broken (the check can fail for reasons outside the
    pack), and the pack may be in use by a running build. The install that
    follows replaces it in the safe order.
    """
    put_back_set_aside(log)     # first: a pack an interrupted install left aside
    for base in shared_cache_roots():
        latest = base / "latest"
        try:
            present = latest.exists() or _is_link(latest)
        except OSError:
            present = False
        if not present:
            continue
        root = unwrap_pack_root(latest)
        if pack_root_looks_usable(root) and toolchain_bin_runs(root / "bin"):
            continue
        if not _is_link(latest) or pack_root_looks_usable(root):
            if log:
                log(f"Toolchain at {latest} did not pass its check; it was not removed.")
            continue
        if _readonly_skip("remove a latest pointer that points at nothing", latest, log):
            continue
        if log:
            log(f"Removing toolchain pointer that points at nothing: {latest}")
        if not _remove_pointer(latest) and log:
            log(f"Could not remove toolchain pointer: {latest}")


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


def _dedupe_paths(paths: list[Path]) -> list[Path]:
    seen: set[str] = set()
    uniq: list[Path] = []
    for r in paths:
        try:
            key = str(r.expanduser())
        except OSError:
            key = str(r)
        if key not in seen:
            seen.add(key)
            uniq.append(Path(key))
    return uniq


def retcomm_data_homes() -> list[Path]:
    """Candidate …/retcomm data roots (RETCOMM_DATA_HOME, XDG, ~/.local/share)."""
    homes: list[Path] = []
    env = (os.environ.get("RETCOMM_DATA_HOME") or "").strip()
    if env:
        homes.append(Path(env).expanduser())
    xdg = (os.environ.get("XDG_DATA_HOME") or "").strip()
    if xdg:
        homes.append(Path(xdg).expanduser() / "retcomm")
    # Always also try the conventional home path — install.sh may have used it
    # even when XDG_DATA_HOME is set to something else (or vice versa).
    homes.append(Path.home() / ".local" / "share" / "retcomm")
    if sys_platform_is_windows():
        local = os.environ.get("LOCALAPPDATA")
        if local:
            homes.append(Path(local) / "retcomm")
    return _dedupe_paths(homes)


def shared_cache_roots() -> list[Path]:
    """Candidate parent dirs that contain <tag>/ packs (or a flat pack).

    Layout matches retcomm-toolchains install.sh:
      …/retcomm/toolchains/cmake-clang-v1/<tag>/   plus latest → current

    Retro (`retcomm`) is preferred; legacy `psxrecomp` remains a read/migrate
    fallback. Honors RETCOMM_TOOLCHAIN_CACHE / RETCOMM_DATA_HOME.
    """
    roots: list[Path] = []
    cache = (os.environ.get("RETCOMM_TOOLCHAIN_CACHE") or "").strip()
    if cache:
        roots.append(Path(cache).expanduser())
    for data in retcomm_data_homes():
        roots.append(data / "toolchains" / PACK_ID)
    # Legacy psxrecomp mirrors beside each retcomm root's parent share.
    for data in retcomm_data_homes():
        parent = data.parent  # …/share or LOCALAPPDATA
        roots.append(parent / "psxrecomp" / "toolchains" / PACK_ID)
    if sys_platform_is_windows():
        local = os.environ.get("LOCALAPPDATA")
        if local:
            roots.append(Path(local) / "psxrecomp" / "toolchains" / PACK_ID)
    return _dedupe_paths(roots)


def preferred_install_root() -> Path:
    """Where newly downloaded / offline-unpacked packs land (Retro shared)."""
    cache = (os.environ.get("RETCOMM_TOOLCHAIN_CACHE") or "").strip()
    if cache:
        r = Path(cache).expanduser()
        r.mkdir(parents=True, exist_ok=True)
        return r
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


# Framework floor, not a per-title pin: v1.0.14 is the first pack with the
# Linux build sysroot (glibc/libstdc++/GL headers + stubs). Older cached packs
# compile against host headers, which do not exist on stock SteamOS, and their
# objects mis-link after a pack upgrade (__isoc23_strtoul). Because the cache
# is accepted forever once any cmake runs, a floor is the only way a stale
# pre-sysroot pack ever gets replaced.
DEFAULT_MIN_VERSION = "1.0.14"

# RETCOMM_TOOLCHAIN_MIN_VERSION values that disable the floor entirely.
_MIN_VERSION_DISABLE = {"0", "off", "none"}


def default_min_version() -> str:
    """Semver floor for accepted packs (DEFAULT_MIN_VERSION unless overridden).

    RETCOMM_TOOLCHAIN_MIN_VERSION overrides in either direction: a version
    raises/lowers the floor, and "0" / "off" / "none" disables it (accept any
    usable pack, matching the pre-floor behavior).
    """
    env = (os.environ.get("RETCOMM_TOOLCHAIN_MIN_VERSION") or "").strip()
    if env:
        return "" if env.lower() in _MIN_VERSION_DISABLE else env
    return DEFAULT_MIN_VERSION


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
    if _readonly_skip("promote the legacy toolchain cache", dest, log):
        return
    aside = None
    try:
        retcomm.mkdir(parents=True, exist_ok=True)
        if _is_link(dest):
            _remove_pointer(dest)
        elif dest.exists():
            aside = _set_aside(dest)
            if aside is None:
                if log:
                    log(f"Could not migrate legacy toolchain cache: {dest} is in use.")
                return
        shutil.copytree(src, dest)
        _remove_aside(aside, log)
        if log:
            log(f"Migrated toolchain cache {src} -> {dest}")
    except OSError as exc:
        if aside is not None and not dest.exists():
            try:
                os.rename(aside, dest)
            except OSError:
                pass
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
    if _readonly_skip("write the project's toolchain stamp", stamp_dir):
        return
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


def pack_python_exe(pack_root: Path) -> Optional[Path]:
    """Return portable CPython under <pack>/python/ (cmake-clang-v1 1.0.6+)."""
    py_root = pack_root / "python"
    if not py_root.is_dir():
        return None
    if sys_platform_is_windows():
        for name in ("python.exe", "python3.exe"):
            cand = py_root / name
            if cand.is_file():
                return cand
    for rel in ("bin/python3", "bin/python"):
        cand = py_root / rel
        if cand.is_file():
            return cand
    # macOS universal pack: arch-specific trees (+ bin/ dispatcher).
    if sys.platform == "darwin":
        machine = platform.machine().lower()
        if machine in ("arm64", "aarch64"):
            arch_rels = (
                "aarch64-apple-darwin/bin/python3",
                "aarch64-apple-darwin/bin/python",
            )
        else:
            arch_rels = (
                "x86_64-apple-darwin/bin/python3",
                "x86_64-apple-darwin/bin/python",
            )
        for rel in arch_rels:
            cand = py_root / rel
            if cand.is_file():
                return cand
    return None


def activate_toolchain_bin(bin_dir: Path, log=None) -> None:
    prefix = str(bin_dir)
    pack_root = bin_dir.parent
    cur = os.environ.get("PATH", "")
    parts = [p for p in (cur.split(os.pathsep) if cur else []) if p]
    # Idempotent session PATH: only prepend when missing.
    path_prefixes: list[str] = []
    py_exe = pack_python_exe(pack_root)
    if py_exe is not None:
        # Windows PBS: <pack>/python/python.exe — Unix: <pack>/python/bin/python3
        py_dir = str(py_exe.parent)
        if py_dir not in parts:
            path_prefixes.append(py_dir)
        os.environ["RETCOMM_PYTHON"] = str(py_exe)
    if str(bin_dir) not in parts and prefix not in parts:
        path_prefixes.append(prefix)
    if path_prefixes:
        os.environ["PATH"] = os.pathsep.join(path_prefixes) + (
            os.pathsep + cur if cur else ""
        )
    # Host deps under deps/ (1.0.9+); legacy packs keep zlib/SDL3 at pack root.
    # Never put pack root on CMAKE_PREFIX_PATH — mingw include/ poisons libc++.
    pack_s = str(pack_root)
    deps_root = pack_root / "deps"
    os.environ["RETCOMM_TOOLCHAIN_DIR"] = pack_s
    if "PSXRECOMP_TOOLCHAIN_DIR" not in os.environ:
        os.environ["PSXRECOMP_TOOLCHAIN_DIR"] = pack_s
    if (deps_root / "include" / "zlib.h").is_file():
        os.environ["ZLIB_ROOT"] = str(deps_root)
    elif (pack_root / "include" / "zlib.h").is_file():
        os.environ["ZLIB_ROOT"] = pack_s
    prev_prefix = os.environ.get("CMAKE_PREFIX_PATH", "")
    if prev_prefix:
        filtered = [
            p for p in prev_prefix.split(os.pathsep)
            if p and os.path.normcase(os.path.normpath(p))
            != os.path.normcase(os.path.normpath(pack_s))
        ]
        os.environ["CMAKE_PREFIX_PATH"] = os.pathsep.join(filtered)
    elif "CMAKE_PREFIX_PATH" in os.environ:
        del os.environ["CMAKE_PREFIX_PATH"]
    sdl3_dir = None
    for root in (deps_root, pack_root):
        cfg = root / "lib" / "cmake" / "SDL3" / "SDL3Config.cmake"
        cfg_alt = root / "lib" / "cmake" / "SDL3" / "SDL3-config.cmake"
        if cfg.is_file() or cfg_alt.is_file():
            sdl3_dir = root / "lib" / "cmake" / "SDL3"
            break
    if sdl3_dir is not None:
        os.environ["SDL3_DIR"] = str(sdl3_dir)
    lib_dir = pack_root / "lib"
    if lib_dir.is_dir() and not sys_platform_is_windows():
        lib_s = str(lib_dir)
        prev_llp = os.environ.get("LD_LIBRARY_PATH", "")
        parts = prev_llp.split(os.pathsep) if prev_llp else []
        if lib_s not in parts:
            os.environ["LD_LIBRARY_PATH"] = lib_s + (
                os.pathsep + prev_llp if prev_llp else ""
            )
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


_MARKER_BEGIN = f"# >>> retcomm-toolchain ({PACK_ID}) >>>"
_MARKER_END = f"# <<< retcomm-toolchain ({PACK_ID}) <<<"


def _strip_marker_block(text: str) -> str:
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    skip = False
    for line in lines:
        stripped = line.rstrip("\n\r")
        if stripped == _MARKER_BEGIN:
            skip = True
            continue
        if stripped == _MARKER_END:
            skip = False
            continue
        if not skip:
            out.append(line)
    return "".join(out)


def _upsert_profile_block(path: Path, block: str) -> None:
    body = ""
    if path.is_file():
        try:
            body = path.read_text(encoding="utf-8")
        except OSError:
            body = ""
    body = _strip_marker_block(body)
    if body and not body.endswith("\n"):
        body += "\n"
    body += block
    if not body.endswith("\n"):
        body += "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")


def _paths_equal(a: Path, b: Path) -> bool:
    try:
        return a.resolve() == b.resolve()
    except OSError:
        return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(
            os.path.normpath(str(b))
        )


def _is_under(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except (ValueError, OSError):
        c = os.path.normcase(os.path.normpath(str(child)))
        p = os.path.normcase(os.path.normpath(str(parent)))
        sep = os.sep
        return c == p or c.startswith(p + sep)


def _set_latest_pointer(cache_root: Path, pack_root: Path) -> Path:
    """Ensure cache_root/latest points at *pack_root* (symlink, else copy).

    The C setup host unpacks cmake-clang-v1 *into* ``…/latest`` as a real
    directory. Do not delete that tree to create a self-pointer (WinError 3).
    """
    latest = cache_root / "latest"
    root = unwrap_pack_root(pack_root)
    # Pack already *is* latest/ (or lives under it) — leave it alone.
    if _paths_equal(root, latest) or _paths_equal(pack_root, latest):
        return root if pack_root_looks_usable(root) else latest
    if (latest.exists() or latest.is_symlink()) and (
        _is_under(root, latest) or _is_under(pack_root, latest)
    ):
        return unwrap_pack_root(latest) if pack_root_looks_usable(
            unwrap_pack_root(latest)
        ) else root

    if _readonly_skip("move the latest pointer", latest):
        return unwrap_pack_root(latest) if pack_root_looks_usable(unwrap_pack_root(latest)) else root
    aside = None
    if _is_link(latest):
        _remove_pointer(latest)
    elif latest.is_dir():
        # A real folder: an older layout's pack, or the copy made below when a
        # link could not be made. Out of use first, removed after the new
        # pointer is there; a folder in use stays, and so does the pointer.
        aside = _set_aside(latest)
        if aside is None:
            return unwrap_pack_root(latest) if pack_root_looks_usable(unwrap_pack_root(latest)) else root
    elif latest.exists():
        latest.unlink(missing_ok=True)
    try:
        try:
            latest.symlink_to(root, target_is_directory=True)
        except OSError:
            shutil.copytree(root, latest)
    except OSError:
        if aside is not None and not latest.exists():
            os.rename(aside, latest)
        raise
    _remove_aside(aside)
    return latest


def _register_user_path_windows(bin_dir: Path, pack_root: Path, log=None) -> None:
    try:
        import winreg  # type: ignore
    except ImportError:
        if log:
            log("Could not import winreg; skipped persistent user PATH")
        return
    bin_s = str(bin_dir)
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, "Environment", 0, winreg.KEY_READ | winreg.KEY_SET_VALUE
        ) as key:
            try:
                raw, _ = winreg.QueryValueEx(key, "Path")
            except FileNotFoundError:
                raw = ""
            parts = [p.rstrip("\\/") for p in str(raw).split(";") if p.strip()]
            if bin_s.rstrip("\\/") not in parts and all(
                p.lower() != bin_s.rstrip("\\/").lower() for p in parts
            ):
                parts.append(bin_s)
                winreg.SetValueEx(key, "Path", 0, winreg.REG_EXPAND_SZ, ";".join(parts))
                if log:
                    log(f"Added to user PATH: {bin_s}")
            elif log:
                log(f"Already on user PATH: {bin_s}")
            winreg.SetValueEx(
                key, "RETCOMM_TOOLCHAIN_DIR", 0, winreg.REG_EXPAND_SZ, str(pack_root)
            )
    except OSError as exc:
        if log:
            log(f"Could not update user PATH: {exc}")
        return
    # Broadcast env change (best-effort).
    try:
        import ctypes

        HWND_BROADCAST = 0xFFFF
        WM_SETTINGCHANGE = 0x001A
        ctypes.windll.user32.SendMessageTimeoutW(
            HWND_BROADCAST, WM_SETTINGCHANGE, 0, "Environment", 0x0002, 5000, None
        )
    except Exception:
        pass


def _register_user_path_unix(cache_root: Path, latest: Path, log=None) -> None:
    hook = cache_root / "path.sh"
    # POSIX sh only, and no sourcing of the pack's env.sh: that script finds
    # itself through ${BASH_SOURCE[0]}, which is empty under zsh (the macOS
    # login shell), so it resolved to the current directory and put $HOME/bin
    # on PATH instead of the pack. The absolute path is known here; use it.
    # Only bin/ (cmake, ninja, ccache) goes on PATH -- the pack's Python stays
    # out of the user's shell, where it would shadow their own python3.
    hook.write_text(
        "# Auto-generated by psxrecomp ensure-toolchain — do not edit.\n"
        f'RETCOMM_TC_LATEST="{latest}"\n'
        'if [ -d "${RETCOMM_TC_LATEST}/bin" ]; then\n'
        '  case ":${PATH}:" in\n'
        '    *":${RETCOMM_TC_LATEST}/bin:"*) ;;\n'
        '    *) export PATH="${RETCOMM_TC_LATEST}/bin${PATH:+:${PATH}}" ;;\n'
        "  esac\n"
        "fi\n"
        'export RETCOMM_TOOLCHAIN_DIR="${RETCOMM_TC_LATEST}"\n',
        encoding="utf-8",
    )
    block = (
        f"{_MARKER_BEGIN}\n"
        f"# Managed by psxrecomp / Retro — remove via uninstall.sh\n"
        f'if [ -f "{hook}" ]; then\n'
        f"  # shellcheck disable=SC1091\n"
        f'  . "{hook}"\n'
        f"fi\n"
        f"{_MARKER_END}\n"
    )
    home = Path.home()
    profiles: list[Path] = []
    for name in (".bashrc", ".zshrc", ".profile"):
        p = home / name
        if p.is_file():
            profiles.append(p)
    if not profiles:
        profiles.append(home / (".zshrc" if sys.platform == "darwin" else ".bashrc"))
    for rc in profiles:
        try:
            _upsert_profile_block(rc, block)
            if log:
                log(f"Updated PATH hook in {rc}")
        except OSError as exc:
            if log:
                log(f"Could not update {rc}: {exc}")


def register_toolchain_user_env(pack_root: Path, log=None) -> Path:
    """Refresh ``latest`` + idempotently add ``latest/bin`` to the user login PATH.

    Mirrors retcomm-toolchains zip ``install.sh`` / ``install.ps1`` so Retro
    setup hosts and the standalone wizard leave cmake/clang on PATH for shell
    development after placing the pack in the shared cache.
    """
    root = unwrap_pack_root(pack_root)
    if not pack_root_looks_usable(root):
        raise RuntimeError(f"toolchain pack unusable for PATH register: {pack_root}")
    if _readonly_skip("refresh the latest pointer and the user's login PATH", root, log):
        activate_toolchain_bin(root / "bin", log=None)     # this process only
        return root
    cache_root = preferred_install_root()
    cache_root.mkdir(parents=True, exist_ok=True)
    try:
        latest = _set_latest_pointer(cache_root, root)
    except OSError as exc:
        # Pointer refresh is best-effort — never discard a usable pack over it.
        if log:
            log(f"Could not refresh latest pointer: {exc}")
        latest = root
    # PATH / env must target a pack with bin/cmake — unwrap nested zip layouts
    # and prefer the real tag dir when latest is only a pointer.
    usable = unwrap_pack_root(latest)
    if not pack_root_looks_usable(usable):
        usable = root
    bin_dir = usable / "bin"
    if not bin_looks_usable(bin_dir):
        bin_dir = root / "bin"
    if sys_platform_is_windows():
        _register_user_path_windows(bin_dir, usable, log=log)
    else:
        _register_user_path_unix(cache_root, usable, log=log)
    # Session PATH for this process (idempotent).
    activate_toolchain_bin(bin_dir if bin_looks_usable(bin_dir) else root / "bin", log=None)
    if log:
        log(f"Registered toolchain user PATH via {bin_dir}")
    return usable


def unpack_zip_to(zip_path: Path, dest: Path) -> Path:
    """Extract *zip_path* into *dest* (replaced) and return usable pack root."""
    if dest.exists():
        shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(dest)
        # zipfile.extractall() discards the Unix mode held in external_attr, so
        # on POSIX hosts every binary lands 0644 and the pack is unusable:
        # "Toolchain cmake probe failed ... [Errno 13] Permission denied".
        # Restore the recorded mode (zips built on Windows record none, in
        # which case the extracted default is already correct).
        if os.name != "nt":
            for info in zf.infolist():
                if info.is_dir():
                    continue
                mode = (info.external_attr >> 16) & 0o7777
                if mode:
                    target = dest / info.filename
                    if target.exists():
                        target.chmod(mode)
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
    if _readonly_skip("install a toolchain pack", zip_path, log):
        raise RuntimeError(
            f"The toolchain is read-only for this start ({READONLY_ENV}). Nothing was installed."
        )
    put_back_set_aside(log)     # a pack an interrupted install left aside, before anything else
    cache_root = preferred_install_root()
    staging = cache_root / ".staging-offline"
    root = unpack_zip_to(zip_path, staging)
    if not pack_satisfies_min(root, min_version):
        need = min_version or default_min_version()
        have = read_pack_version(root) or "(unknown)"
        shutil.rmtree(staging, ignore_errors=True)
        raise RuntimeError(
            f"Toolchain zip version {have} does not meet min_version {need}."
        )
    # 1. The new pack is checked where it was unpacked, before the installed
    #    one is touched. The same steps as the setup host's check.
    step = toolchain_bin_check(root / "bin", log=log)
    if step:
        shutil.rmtree(staging, ignore_errors=True)
        raise ToolchainRefused(_new_pack_failed_text(step))
    dest_tag = _install_tag_for_root(root, tag)
    dest = cache_root / dest_tag
    aside = None
    moved = dest.resolve() != root.resolve()
    if moved:
        # 2. The installed folder of this tag goes out of use by a rename.
        if _is_link(dest):
            _remove_pointer(dest)
        elif dest.exists():
            aside = _set_aside(dest)
            if aside is None:
                shutil.rmtree(staging, ignore_errors=True)
                raise ToolchainRefused(_rename_failed_text(_aside_error))
        # 3. The new pack takes the name.
        try:
            shutil.move(str(root), str(dest))
        except OSError:
            # Put the installed pack back under its name.
            shutil.rmtree(dest, ignore_errors=True)
            if aside is not None and not dest.exists():
                os.rename(aside, dest)
            shutil.rmtree(staging, ignore_errors=True)
            raise
        root = unwrap_pack_root(dest)
        # Drop empty staging parent left behind by a nested unzip layout.
        if staging.exists() and staging != dest:
            shutil.rmtree(staging, ignore_errors=True)
    # 4. The pointer follows, and the pack is checked again in its place.
    try:
        _set_latest_pointer(cache_root, root)
    except OSError as exc:
        if log:
            log(f"Could not refresh latest pointer: {exc}")
    step = toolchain_bin_check(root / "bin", log=log)
    if step:
        # It passed where it was unpacked and fails in its place: the new pack
        # is taken out and the pack that was installed goes back under its name.
        if moved:
            shutil.rmtree(dest, ignore_errors=True)
            if aside is not None and not dest.exists():
                try:
                    os.rename(aside, dest)
                    _set_latest_pointer(cache_root, unwrap_pack_root(dest))
                except OSError as exc:
                    if log:
                        log(f"Could not put the installed toolchain back: {exc}")
            elif _is_link(cache_root / "latest") and not pack_root_looks_usable(unwrap_pack_root(cache_root / "latest")):
                _remove_pointer(cache_root / "latest")
        raise ToolchainRefused(_new_pack_failed_text(step))
    # 5. Only now the folder that was set aside is removed, the user's login
    #    PATH is written (same as zip install.sh) and older tags are pruned.
    _remove_aside(aside, log)
    register_toolchain_user_env(root, log=log)
    prune_old_toolchain_tags(root, log=log)
    if project_root is not None:
        return materialize_into_project(project_root, root, log=log)
    return root


def download_url(url: str, dest: Path, token: Optional[str] = None) -> None:
    # Lazy: keep urllib off the import path so `python tools/...` still works
    # when ambient PYTHONHOME breaks _socket (launcher clears PYTHONHOME on
    # spawn).
    import urllib.request

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
    import urllib.error

    art = artifact or host_artifact()
    asset = _ASSET.get(art)
    if not asset:
        raise RuntimeError(f"unknown toolchain artifact: {art}")
    if _readonly_skip("download a toolchain pack", asset, log):
        raise RuntimeError(
            f"The toolchain is read-only for this start ({READONLY_ENV}). Nothing was downloaded."
        )
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
        # Tag fallback is "offline" — never install *into* latest/; latest is
        # pointer-only (symlink/copy) matching retcomm-toolchains install.sh.
        return install_from_zip(
            zpath,
            tag="offline",
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

    Installs always land under the shared Retro cache; the project only gets
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
        heal_broken_toolchain_pointers(log=log)
        clear_project_toolchain_stamp(project_root)
        root = install_from_zip(
            Path(from_zip),
            project_root=project_root,
            min_version=need,
            log=log,
        )
        bin_dir = unwrap_pack_root(root) / "bin"
        if not bin_looks_usable(bin_dir):
            bin_dir = root / "bin"
        if not toolchain_bin_runs(bin_dir, log=log):
            raise RuntimeError(f"Toolchain zip installed but cmake is unusable: {bin_dir}")
        if project_root is not None:
            write_toolchain_stamp(project_root, bin_dir)
        activate_toolchain_bin(bin_dir, log=log)
        return bin_dir

    existing = resolve_toolchain_bin(project_root, min_version=need)
    if existing and toolchain_bin_runs(existing, log=log):
        if project_root is not None:
            write_toolchain_stamp(project_root, existing)
        # Cached pack: still publish latest + user PATH (idempotent).
        # PATH register must not fail ensure (e.g. Store Python AppData quirks).
        try:
            register_toolchain_user_env(existing.parent, log=log)
        except Exception as exc:  # noqa: BLE001 — best-effort side effect
            if log:
                log(f"PATH register skipped: {exc}")
        activate_toolchain_bin(existing, log=log)
        return existing

    if existing and log:
        log(
            f"Cached toolchain at {existing} is missing or broken; "
            "will repair."
        )

    # Usable pack exists but is too old — fall through to download/replace.
    # min_version="0" probes without the default floor ("" would re-apply it).
    stale = resolve_toolchain_bin(project_root, min_version="0")
    if stale and need and not existing and log:
        log(
            f"Cached toolchain does not meet min_version {need}; "
            "will download/replace."
        )

    # Drop bad latest/ pointers and project stamps before reinstall.
    heal_broken_toolchain_pointers(log=log)
    clear_project_toolchain_stamp(project_root)

    if download:
        if log:
            log("Downloading portable cmake/clang toolchain…")
        root = download_latest_pack(
            repo=repo, log=log, project_root=project_root, min_version=need
        )
        bin_dir = unwrap_pack_root(root) / "bin"
        if not bin_looks_usable(bin_dir):
            bin_dir = root / "bin"
        if not toolchain_bin_runs(bin_dir, log=log):
            raise RuntimeError(
                f"Downloaded toolchain but cmake is unusable: {bin_dir}"
            )
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
