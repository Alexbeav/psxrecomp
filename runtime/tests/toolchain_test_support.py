"""Shared by the toolchain tests (PS1B-410): a closed environment, a made-up cache,
and a probe that runs one toolchain operation of the setup host.

The setup host (host/psxrecomp_codegen_host.c) and the CLI (tools/toolchain_pack.py)
each hold a toolchain installer. A test of them must not be able to reach the
machine's own toolchain: on 2026-10-02 a setup host started by a test installed
a pack over a build host's own and emptied it under running builds (PS1B-406).
So every child of these tests gets closed_environment(): every folder a pack or
a pointer is looked up in (LOCALAPPDATA, APPDATA, USERPROFILE, HOME,
XDG_DATA_HOME, RETCOMM_DATA_HOME, RETCOMM_TOOLCHAIN_CACHE, TEMP, TMP) is a
folder inside the test's temporary folder, PATH holds the system folders only,
no variable names a toolchain, and the proxy variables point at a closed port.
The CLI's functions that write the user's login PATH (the registry on Windows,
the shell profile elsewhere) are replaced by recorders in load_cli().
"""

from __future__ import annotations

from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[2]
HOST_C = ROOT / "host" / "psxrecomp_codegen_host.c"
SWITCH = "PSXRECOMP_TOOLCHAIN_READONLY"
WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
CMAKE = "cmake" + EXE
PACK_OS = "windows-x64" if WINDOWS else ("macos-universal" if sys.platform == "darwin" else "linux-x64")

SANDBOX_ROOTS = {
    "LOCALAPPDATA": "localappdata",
    "APPDATA": "appdata",
    "USERPROFILE": "home",
    "HOME": "home",
    "XDG_DATA_HOME": "xdg-data",
    "RETCOMM_DATA_HOME": "retcomm-data",
    "RETCOMM_TOOLCHAIN_CACHE": "retcomm-toolchain-cache",
}
DEAD_PROXY = "http://127.0.0.1:9"


def closed_environment(sandbox: Path) -> dict[str, str]:
    """The only environment a child of these tests gets. Built from nothing."""
    env: dict[str, str] = {}
    if WINDOWS:
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        env["SystemRoot"] = system_root
        env["windir"] = system_root
        env["SystemDrive"] = os.environ.get("SystemDrive", "C:")
        env["ComSpec"] = str(Path(system_root) / "System32" / "cmd.exe")
        env["PATHEXT"] = ".COM;.EXE;.BAT;.CMD"
        env["PATH"] = os.pathsep.join([str(Path(system_root) / "System32"), system_root])
    else:
        env["PATH"] = "/usr/bin:/bin"
    for name, folder in SANDBOX_ROOTS.items():
        (sandbox / folder).mkdir(parents=True, exist_ok=True)
        env[name] = str(sandbox / folder)
    (sandbox / "temp").mkdir(parents=True, exist_ok=True)
    for name in ("TEMP", "TMP", "TMPDIR"):
        env[name] = str(sandbox / "temp")
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        env[name] = DEAD_PROXY
        if not WINDOWS:
            env[name.lower()] = DEAD_PROXY
    return env


def roots_outside(env, sandbox: Path) -> list[str]:
    """Names of the toolchain, data and temp roots that are not inside `sandbox`."""
    inside = sandbox.resolve()
    return [name for name in list(SANDBOX_ROOTS) + ["TEMP", "TMP", "TMPDIR"]
            if name not in env or inside not in Path(env[name]).resolve().parents]


def dry_run() -> int:
    """Prints the environment a child would get. Builds nothing, starts nothing."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        sandbox = Path(tmp) / "sandbox"
        env = closed_environment(sandbox)
        print("dry run: nothing is built and no program is started")
        for name in sorted(env):
            print("  %s=%s" % (name, env[name]))
        outside = roots_outside(env, sandbox)
        print("roots outside the temporary folder: %s" % (", ".join(outside) if outside else "none"))
        return 1 if outside else 0


def other_roots_entries(sandbox: Path) -> list[str]:
    """What appeared in the data roots other than the cache the test works on."""
    return sorted(str(Path(folder) / p) for folder in sorted(set(SANDBOX_ROOTS.values()))
                  if folder != "retcomm-toolchain-cache" for p in snapshot(sandbox / folder))


# --- a made-up cache ---------------------------------------------------------------------------

PACK_TOOLS = ("cmake", "clang", "ld.lld")


def pack_manifest(version: str) -> str:
    return json.dumps({"id": "cmake-clang-v1", "version": version, "os": PACK_OS})


def make_pack(root: Path, version: str = "1.0.14", marker: str = "", stub: Path | None = None) -> Path:
    """A pack. Without `stub` its cmake is a file that does not run: the pack is
    there and fails its check. With `stub` (a program that exits 0) cmake, clang
    and ld.lld run, so the pack passes."""
    (root / "bin").mkdir(parents=True)
    if stub is None:
        (root / "bin" / CMAKE).write_text("not a program\n", encoding="utf-8")
    else:
        for tool in PACK_TOOLS:
            shutil.copy2(stub, root / "bin" / (tool + EXE))
    (root / "retcomm-toolchain.json").write_text(pack_manifest(version), encoding="utf-8")
    if marker:
        (root / marker).write_text(marker + "\n", encoding="utf-8")
    return root


def make_zip(path: Path, version: str = "1.0.14", marker: str = "new.txt", stub: Path | None = None) -> Path:
    """The zip of a pack, with the Unix modes a real pack's zip records."""
    with zipfile.ZipFile(path, "w") as archive:
        def put(name: str, data: bytes, mode: int) -> None:
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = mode << 16
            archive.writestr(info, data)
        if stub is None:
            put("bin/" + CMAKE, b"not a program\n", 0o644)
        else:
            for tool in PACK_TOOLS:
                put("bin/" + tool + EXE, stub.read_bytes(), 0o755)
        put("retcomm-toolchain.json", pack_manifest(version).encode("utf-8"), 0o644)
        put(marker, (marker + "\n").encode("utf-8"), 0o644)
    return path


def make_stub(tmp: Path, cc: str | None) -> Path | None:
    """A program that exits 0 whatever its arguments: stands for cmake, clang and
    ld.lld of a pack that passes its check. None on Windows without a compiler."""
    if not WINDOWS:
        stub = tmp / "stub"
        stub.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        stub.chmod(0o755)
        return stub
    if cc is None:
        return None
    source = tmp / "stub.c"
    source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    stub = tmp / "stub.exe"
    build = subprocess.run([cc, "-o", str(stub), str(source)], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    return stub if build.returncode == 0 and stub.is_file() else None


def snapshot(top: Path) -> dict[str, str]:
    """Every entry below `top` with the hash of its bytes."""
    found: dict[str, str] = {}
    if not os.path.lexists(top):
        return found
    for folder, names, files in os.walk(top):
        for name in names:
            path = Path(folder) / name
            found[str(path.relative_to(top))] = "link" if is_link(path) else "dir"
        for name in files:
            path = Path(folder) / name
            try:
                found[str(path.relative_to(top))] = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError:
                found[str(path.relative_to(top))] = "unreadable"
    return found


def is_link(path: Path) -> bool:
    """A symlink or a Windows junction."""
    try:
        if path.is_symlink():
            return True
        os.readlink(path)
        return True
    except (OSError, ValueError):
        return False


def make_link(link: Path, target: Path) -> bool:
    """A directory link the way the installers make one: a symlink, or a junction on Windows."""
    try:
        link.symlink_to(target, target_is_directory=True)
        return True
    except OSError:
        if not WINDOWS:
            return False
    made = subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(target)], capture_output=True)
    return made.returncode == 0


# --- the CLI's module ------------------------------------------------------------------------------

def load_cli(calls: list):
    """tools/toolchain_pack.py with the two functions that write the user's login
    PATH replaced by recorders. Refuses when a root is outside TEST_SANDBOX."""
    sandbox = Path(os.environ["TEST_SANDBOX"])
    outside = roots_outside(dict(os.environ), sandbox)
    if outside:
        raise SystemExit("refused: roots outside the sandbox: %s" % ", ".join(outside))
    sys.path.insert(0, str(ROOT / "tools"))
    import toolchain_pack as tp
    tp._register_user_path_windows = lambda *a, **k: calls.append("windows")     # the registry
    tp._register_user_path_unix = lambda *a, **k: calls.append("unix")           # the shell profile
    return tp, sandbox


def run_cli_child(test_file: Path, case: str, sandbox: Path, switch: bool, extra_env: dict | None = None) -> dict:
    """Runs `<test_file> --cli-child <case>` in the closed environment; returns what it printed."""
    env = closed_environment(sandbox)
    env["TEST_SANDBOX"] = str(sandbox)
    if switch:
        env[SWITCH] = "1"
    env.update(extra_env or {})
    run = subprocess.run([sys.executable, str(test_file), "--cli-child", case], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", env=env, cwd=str(sandbox))
    try:
        seen = json.loads(run.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        seen = {"broken": (run.stdout + run.stderr)[-1500:]}
    seen["stderr"] = run.stderr
    return seen


# --- the setup host, by a probe that includes its source file ------------------------------------

PROBE = r"""
#include "%(host_c)s"
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
/* probe <operation> [argument] [argument]: one toolchain operation of the host. */
int main(int argc, char** argv) {
    char err[512];
    const char* op = argc > 1 ? argv[1] : "";
    const char* a = argc > 2 ? argv[2] : "";
    const char* b = argc > 3 ? argv[3] : "";
    const char* project = getenv("PROBE_PROJECT");
    int ret = -1;
    err[0] = '\0';
    if (project)
        snprintf(g_project_root, sizeof(g_project_root), "%%s", project);
    if (strcmp(op, "heal") == 0) {
        heal_broken_toolchain_pointers();
        ret = 0;
    } else if (strcmp(op, "discard") == 0) {
        snprintf(g_toolchain_bin, sizeof(g_toolchain_bin), "%%s", a);
        discard_unhealthy_active_toolchain();
        ret = 0;
    } else if (strcmp(op, "prune") == 0) {
        prune_old_toolchain_tags(a);
        ret = 0;
    } else if (strcmp(op, "install") == 0) {
        ret = host_install_toolchain_from_zip(a, NULL, NULL, err, sizeof(err));
    } else if (strcmp(op, "download") == 0) {
        ret = host_download_and_install_toolchain(NULL, NULL, err, sizeof(err));
    } else if (strcmp(op, "pointer") == 0) {
        ret = set_toolchain_latest_pointer(a, b);
    } else if (strcmp(op, "stamp") == 0) {
        clear_project_toolchain_stamp();
        ret = write_project_toolchain_stamp(a);
    } else if (strcmp(op, "link") == 0) {
        ret = link_or_stamp_project_toolchain(a);
    } else if (strcmp(op, "healthy") == 0) {
        ret = toolchain_bin_is_healthy(a);
    } else if (strcmp(op, "activate") == 0) {
        /* activate_toolchain_path <n> times; PATH's length after each, and PATH itself when it is short. */
        int n = atoi(a), i;
        for (i = 0; i < n; ++i) {
            activate_toolchain_path();
            printf("len%%d=%%u\n", i + 1, (unsigned)strlen(getenv("PATH") ? getenv("PATH") : ""));
        }
        if (getenv("PATH") && strlen(getenv("PATH")) < 3000)
            printf("path=%%s\n", getenv("PATH"));
        printf("bin=%%s\n", g_toolchain_bin);
        ret = g_toolchain_bin[0] ? 1 : 0;
    } else if (strcmp(op, "resolve") == 0) {
        /* What the lookup finds, before and after the pack in use failed its check. */
        char first[1400], second[1400];
        first[0] = second[0] = '\0';
        resolve_toolchain_bin(first, sizeof(first));
        snprintf(g_toolchain_bin, sizeof(g_toolchain_bin), "%%s", first);
        discard_unhealthy_active_toolchain();
        resolve_toolchain_bin(second, sizeof(second));
        printf("first=%%s\nsecond=%%s\nnote=%%s\n", first, second, g_tc_repair_note);
        ret = 0;
    } else {
        fprintf(stderr, "probe: unknown operation\n");
        return 2;
    }
    printf("ret=%%d\nerr=%%s\n", ret, err);
    return 0;
}
"""


def find_recomp_ui() -> Path | None:
    env = os.environ.get("RECOMP_UI_ROOT")
    for c in ([Path(env)] if env else []) + [ROOT.parent / "recomp-ui", ROOT / "recomp-ui"]:
        if (c / "src" / "recomp_launcher.h").is_file():
            return c
    return None


def find_compiler(argv: list[str]) -> str | None:
    if "--compiler" in argv:
        return argv[argv.index("--compiler") + 1]
    return os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")


def build_probe(tmp: Path, cc: str | None) -> tuple[Path | None, str]:
    """(the probe, '') or (None, why the host layer is skipped or failed)."""
    ui = find_recomp_ui()
    if ui is None:
        return None, "SKIP: recomp-ui not found (set RECOMP_UI_ROOT)"
    if cc is None:
        return None, "SKIP: no C compiler"
    probe_c = tmp / "probe.c"
    probe_c.write_text(PROBE % {"host_c": str(HOST_C).replace("\\", "/")}, encoding="utf-8")
    probe = tmp / ("probe" + EXE)
    build = subprocess.run(
        [cc, "-std=c11", "-o", str(probe), str(probe_c), '-DPSX_SETUP_BIOS_STEMS="SCPH5552"',
         '-DPSX_SETUP_FRAMEWORK_REL="psxrecomp"', "-I", str(ROOT / "host"), "-I", str(ROOT / "runtime" / "include"),
         "-I", str(ui / "src"), "-I", str(ui / "src" / "common")],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if build.returncode != 0:
        return None, "FAIL: could not build the host probe\n%s" % build.stderr[-2500:]
    return probe, ""


def run_probe(probe: Path, args: list[str], sandbox: Path, switch: bool) -> dict:
    """Runs the probe in the closed environment of `sandbox`. The project is <sandbox>/project."""
    env = closed_environment(sandbox)
    if switch:
        env[SWITCH] = "1"
    outside = roots_outside(env, sandbox)
    if outside:
        return {"refused": "roots outside the sandbox: %s" % ", ".join(outside)}
    (sandbox / "project").mkdir(exist_ok=True)
    env["PROBE_PROJECT"] = str(sandbox / "project")
    run = subprocess.run([str(probe)] + args, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         env=env, cwd=str(sandbox))
    lines = dict(line.split("=", 1) for line in run.stdout.splitlines() if "=" in line)
    lines.update({"stderr": run.stderr, "exit": run.returncode})
    return lines


class Checks:
    """Counts failures and prints one line per check."""

    def __init__(self) -> None:
        self.failures = 0

    def __call__(self, ok: bool, why: str, detail: object = "") -> bool:
        if ok:
            print("ok: %s" % why)
        else:
            self.failures += 1
            print("FAIL: %s\n  %s" % (why, detail))
        return bool(ok)
