"""Local CMake configure / build / launch for game repos.

Cross-platform (Windows / macOS / Linux). No force flags; builds stay under
the chosen build directory (default ``build-release``).
"""

from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from .gitops import CmdResult

DEFAULT_BUILD_DIR = "build-release"
DEFAULT_TARGET = "psx-runtime"
DEFAULT_BUILD_TYPE = "Release"
LogFn = Callable[[str], None]


@dataclass
class BuildHost:
    system: str  # Windows | Darwin | Linux | …
    label: str  # windows | macos | linux | other
    cmake: str | None
    ninja: str | None
    jobs: int


@dataclass
class LaunchHandle:
    proc: subprocess.Popen
    exe: Path
    cwd: Path
    env_overlay: dict[str, str] = field(default_factory=dict)

    @property
    def pid(self) -> int | None:
        return self.proc.pid if self.proc else None

    def poll(self) -> int | None:
        return self.proc.poll() if self.proc else None

    def terminate(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


_active_launch: LaunchHandle | None = None
_launch_lock = threading.Lock()


def detect_host() -> BuildHost:
    system = platform.system()
    if system == "Windows":
        label = "windows"
    elif system == "Darwin":
        label = "macos"
    elif system == "Linux":
        label = "linux"
    else:
        label = "other"
    jobs = os.cpu_count() or 4
    return BuildHost(
        system=system,
        label=label,
        cmake=shutil.which("cmake"),
        ninja=shutil.which("ninja") or shutil.which("ninja-build"),
        jobs=jobs,
    )


def default_generator(host: BuildHost | None = None) -> str:
    host = host or detect_host()
    if host.ninja:
        return "Ninja"
    if host.label == "windows":
        # Leave empty → cmake picks VS / default generator.
        return ""
    return "Unix Makefiles"


def parse_env_text(text: str) -> dict[str, str]:
    """Parse ``KEY=VAL`` pairs from free text (space / newline / ``;`` separated).

    Values may be quoted with single or double quotes. Lines starting with ``#``
    are ignored.
    """
    env: dict[str, str] = {}
    if not text or not text.strip():
        return env
    # Normalize separators to newlines, but keep quoted spans intact via a
    # simple token walk on KEY=VAL forms.
    cleaned: list[str] = []
    for raw_line in text.replace(";", "\n").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        cleaned.append(line)
    blob = "\n".join(cleaned)
    # Match KEY=VALUE where VALUE is "…", '…', or non-space / until next KEY=
    pattern = re.compile(
        r"""(?P<key>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?P<val>"[^"]*"|'[^']*'|\S+)"""
    )
    for m in pattern.finditer(blob):
        key = m.group("key")
        val = m.group("val")
        if (val.startswith('"') and val.endswith('"')) or (
            val.startswith("'") and val.endswith("'")
        ):
            val = val[1:-1]
        env[key] = val
    return env


def _run_stream(
    cmd: list[str],
    cwd: Path,
    *,
    log: LogFn | None = None,
    env: dict[str, str] | None = None,
) -> CmdResult:
    if log:
        log("$ " + " ".join(cmd))
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
            bufsize=1,
        )
    except OSError as exc:
        return CmdResult(False, f"Failed to start: {cmd[0]}", str(exc))

    assert proc.stdout is not None
    lines: list[str] = []
    for line in proc.stdout:
        line = line.rstrip("\n")
        lines.append(line)
        if log:
            log(line)
    code = proc.wait()
    detail = "\n".join(lines[-40:])
    if code != 0:
        return CmdResult(False, f"Command failed (exit {code})", detail)
    return CmdResult(True, "OK", detail)


def configure(
    root: Path,
    *,
    build_dir: str = DEFAULT_BUILD_DIR,
    build_type: str = DEFAULT_BUILD_TYPE,
    generator: str | None = None,
    extra_args: list[str] | None = None,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    root = root.expanduser().resolve()
    host = detect_host()
    if not host.cmake:
        return CmdResult(False, "cmake not found on PATH")
    if not (root / "CMakeLists.txt").is_file():
        return CmdResult(False, f"No CMakeLists.txt in {root}")

    bdir = Path(build_dir)
    if not bdir.is_absolute():
        bdir = root / bdir
    gen = generator if generator is not None else default_generator(host)
    cmd = [host.cmake, "-S", str(root), "-B", str(bdir)]
    if gen:
        cmd.extend(["-G", gen])
    cmd.append(f"-DCMAKE_BUILD_TYPE={build_type}")
    if extra_args:
        cmd.extend(extra_args)

    if dry_run:
        msg = "dry-run: " + " ".join(cmd)
        if log:
            log(msg)
        return CmdResult(True, msg)

    r = _run_stream(cmd, root, log=log)
    if r.ok:
        r = CmdResult(True, f"Configured {bdir.name} ({build_type}" + (f", {gen}" if gen else "") + ")", r.detail)
    return r


def build(
    root: Path,
    *,
    build_dir: str = DEFAULT_BUILD_DIR,
    target: str = DEFAULT_TARGET,
    jobs: int | None = None,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    root = root.expanduser().resolve()
    host = detect_host()
    if not host.cmake:
        return CmdResult(False, "cmake not found on PATH")
    bdir = Path(build_dir)
    if not bdir.is_absolute():
        bdir = root / bdir
    if not bdir.is_dir():
        return CmdResult(False, f"Build dir missing — Configure first: {bdir}")

    j = jobs if jobs and jobs > 0 else host.jobs
    cmd = [host.cmake, "--build", str(bdir), "--target", target, "-j", str(j)]
    if dry_run:
        msg = "dry-run: " + " ".join(cmd)
        if log:
            log(msg)
        return CmdResult(True, msg)

    r = _run_stream(cmd, root, log=log)
    if r.ok:
        exe = find_runtime_exe(bdir)
        hint = f" → {exe.name}" if exe else ""
        r = CmdResult(True, f"Built {target} in {bdir.name}{hint}", r.detail)
    return r


def find_runtime_exe(build_dir: Path) -> Path | None:
    """Locate the game product binary under a CMake build tree."""
    build_dir = build_dir.expanduser().resolve()
    if not build_dir.is_dir():
        return None

    host = detect_host()
    suffixes = {""}
    if host.label == "windows":
        suffixes = {".exe"}

    # Prefer names that look like Recompiled products / known targets.
    ranked: list[tuple[int, Path]] = []
    skip_dirs = {
        "CMakeFiles",
        "_deps",
        ".cmake",
        "Testing",
        "CMakeTmp",
        "assets",
        "bios",
        "fonts",
        "img",
        "mods",
    }

    def consider(p: Path) -> None:
        if not p.is_file():
            return
        name = p.name
        lower = name.lower()
        if host.label == "windows":
            if not lower.endswith(".exe"):
                return
            stem = name[:-4]
        else:
            if any(
                lower.endswith(ext)
                for ext in (".so", ".dll", ".dylib", ".a", ".lib", ".pdb", ".cmake", ".ninja")
            ):
                return
            stem = name
        if stem.lower() in ("cmake", "ninja", "cpack", "ctest"):
            return
        score = 0
        if "recompil" in lower:
            score += 100
        if stem in ("psx-runtime", "psx-runtime.exe") or stem == "psx-runtime":
            score += 50
        if p.parent == build_dir:
            score += 20
        if host.label != "windows" and not os.access(p, os.X_OK):
            return
        ranked.append((score, p))

    for p in build_dir.iterdir():
        if p.is_file():
            consider(p)

    for sub in build_dir.iterdir():
        if not sub.is_dir() or sub.name in skip_dirs or sub.name.startswith("."):
            continue
        if sub.name in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel") or host.label == "windows":
            for p in sub.iterdir():
                if p.is_file():
                    consider(p)

    if not ranked:
        return None
    ranked.sort(key=lambda t: (-t[0], t[1].name.lower()))
    return ranked[0][1]


def resolve_build_dir(root: Path, build_dir: str) -> Path:
    root = root.expanduser().resolve()
    bdir = Path(build_dir)
    if not bdir.is_absolute():
        bdir = root / bdir
    return bdir


def launch(
    root: Path,
    *,
    build_dir: str = DEFAULT_BUILD_DIR,
    exe: Path | str | None = None,
    env_text: str = "",
    extra_args: list[str] | None = None,
    dry_run: bool = False,
    log: LogFn | None = None,
) -> CmdResult:
    """Start the local product build (non-blocking)."""
    global _active_launch
    root = root.expanduser().resolve()
    bdir = resolve_build_dir(root, build_dir)
    exe_path = Path(exe) if exe else find_runtime_exe(bdir)
    if exe_path is None:
        return CmdResult(False, f"No runtime executable found under {bdir}")
    if not exe_path.is_file():
        return CmdResult(False, f"Executable missing: {exe_path}")

    overlay = parse_env_text(env_text)
    env = os.environ.copy()
    env.update(overlay)
    # Run from game root so relative game.toml / disc / saves resolve.
    cmd = [str(exe_path), *(extra_args or [])]

    if dry_run:
        preview = " ".join(f"{k}={v}" for k, v in overlay.items())
        msg = "dry-run: " + (f"env {preview} " if preview else "") + " ".join(cmd)
        if log:
            log(msg)
        return CmdResult(True, msg)

    with _launch_lock:
        if _active_launch and _active_launch.poll() is None:
            return CmdResult(
                False,
                f"Already running (pid {_active_launch.pid}) — Stop first",
            )
        try:
            kwargs: dict = {
                "cwd": str(root),
                "env": env,
            }
            host = detect_host()
            if host.label == "windows":
                # Detach from Studio console; GUI apps don't need a console.
                kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
                kwargs["stdout"] = subprocess.DEVNULL
                kwargs["stderr"] = subprocess.DEVNULL
            else:
                kwargs["start_new_session"] = True
                kwargs["stdout"] = subprocess.DEVNULL
                kwargs["stderr"] = subprocess.DEVNULL
            proc = subprocess.Popen(cmd, **kwargs)
        except OSError as exc:
            return CmdResult(False, "Launch failed", str(exc))
        _active_launch = LaunchHandle(
            proc=proc, exe=exe_path, cwd=root, env_overlay=overlay
        )

    env_note = ""
    if overlay:
        env_note = " env=[" + ", ".join(sorted(overlay)) + "]"
    msg = f"Launched {exe_path.name} (pid {proc.pid}){env_note}"
    if log:
        log(msg)
    return CmdResult(True, msg)


def stop_launch() -> CmdResult:
    global _active_launch
    with _launch_lock:
        h = _active_launch
        if h is None or h.poll() is not None:
            _active_launch = None
            return CmdResult(False, "No running launch")
        pid = h.pid
        h.terminate()
        try:
            h.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            h.proc.kill()
        _active_launch = None
    return CmdResult(True, f"Stopped pid {pid}")


def launch_status() -> str:
    with _launch_lock:
        h = _active_launch
        if h is None:
            return "not running"
        code = h.poll()
        if code is None:
            return f"running pid={h.pid} ({h.exe.name})"
        return f"exited code={code} ({h.exe.name})"
