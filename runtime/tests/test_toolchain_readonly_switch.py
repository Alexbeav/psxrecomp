#!/usr/bin/env python3
"""PSXRECOMP_TOOLCHAIN_READONLY=1: nothing deletes, renames, prunes or installs a
toolchain pack, and no pointer to one moves.

The setup host (host/psxrecomp_codegen_host.c) and the CLI (tools/toolchain_pack.py)
each hold a toolchain installer. On 2026-10-02 a setup host started by a test
installed a pack over a build host's own and emptied it under running builds
(PS1B-406, PS1B-410). With the switch set a start can still find and use a pack,
and it cannot change one. Tests and gates that start a setup program set it.

Each operation is run twice on a made-up cache: with the switch (the cache must
be byte-for-byte what it was) and without it (the control: the operation is
real, so the first result means something).

  the CLI's module, by its functions:   remove an unusable `latest`, prune older
      installs, install from a zip, move the `latest` pointer, remove and write
      the project stamp, write the user's login PATH, promote the legacy cache;
  the setup host, by a probe that includes its source file:   the same, and the
      two things only it does: set the pack in use aside (rename to <tag>.broken)
      and download.

Everything runs in a closed environment: every folder a pack or a pointer is
looked up in (LOCALAPPDATA, APPDATA, USERPROFILE, HOME, XDG_DATA_HOME,
RETCOMM_DATA_HOME, RETCOMM_TOOLCHAIN_CACHE, TEMP, TMP) is a folder inside the
test's temporary folder, PATH holds the system folders only, the proxy variables
point at a closed port, and the functions that write the user's login PATH (the
registry on Windows, the shell profile elsewhere) are replaced by recorders
before anything runs. --dry-run prints that environment and starts nothing.

The host layer needs recomp-ui (RECOMP_UI_ROOT) and a C compiler; without them
it is skipped and says so.
"""

from __future__ import annotations

from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
HOST_C = ROOT / "host" / "psxrecomp_codegen_host.c"
SWITCH = "PSXRECOMP_TOOLCHAIN_READONLY"
WINDOWS = os.name == "nt"
CMAKE = "cmake.exe" if WINDOWS else "cmake"
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
    """The only environment a child of this test gets. Built from nothing."""
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


def roots_outside(env: dict[str, str], sandbox: Path) -> list[str]:
    inside = sandbox.resolve()
    return [name for name in list(SANDBOX_ROOTS) + ["TEMP", "TMP", "TMPDIR"]
            if name not in env or inside not in Path(env[name]).resolve().parents]


def dry_run() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        sandbox = Path(tmp) / "sandbox"
        env = closed_environment(sandbox)
        print("dry run: nothing is built and no program is started")
        for name in sorted(env):
            print("  %s=%s" % (name, env[name]))
        outside = roots_outside(env, sandbox)
        print("roots outside the temporary folder: %s" % (", ".join(outside) if outside else "none"))
        return 1 if outside else 0


# --- a made-up cache ---------------------------------------------------------------------------

def make_pack(root: Path, version: str = "1.0.14", marker: str = "") -> Path:
    """A pack whose cmake is a file that does not run: present, and not healthy."""
    (root / "bin").mkdir(parents=True)
    (root / "bin" / CMAKE).write_text("not a program\n", encoding="utf-8")
    (root / "retcomm-toolchain.json").write_text(
        json.dumps({"id": "cmake-clang-v1", "version": version, "os": PACK_OS}), encoding="utf-8")
    if marker:
        (root / marker).write_text(marker + "\n", encoding="utf-8")
    return root


def make_zip(path: Path, version: str = "1.0.14", marker: str = "new.txt") -> Path:
    with zipfile.ZipFile(path, "w") as archive:
        for name, text in ((("bin/" + CMAKE), "not a program\n"),
                           ("retcomm-toolchain.json",
                            json.dumps({"id": "cmake-clang-v1", "version": version, "os": PACK_OS})),
                           (marker, marker + "\n")):
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = 0o644 << 16
            archive.writestr(info, text)
    return path


def snapshot(top: Path) -> dict[str, str]:
    """Every entry below `top` with the hash of its bytes (a link: its target's name)."""
    found: dict[str, str] = {}
    if not top.exists():
        return found
    for folder, names, files in os.walk(top):
        for name in names:
            path = Path(folder) / name
            found[str(path.relative_to(top))] = "link" if path.is_symlink() else "dir"
        for name in files:
            path = Path(folder) / name
            try:
                found[str(path.relative_to(top))] = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError:
                found[str(path.relative_to(top))] = "unreadable"
    return found


# --- the CLI's module, run in a child of this file -------------------------------------------------

PY_CASES = ("heal", "prune", "install", "pointer", "stamp", "user_path", "migrate")


def py_child(case: str) -> int:
    """Runs one operation of tools/toolchain_pack.py on a made-up cache and prints what it saw."""
    sandbox = Path(os.environ["TEST_SANDBOX"])
    outside = roots_outside(dict(os.environ), sandbox)
    if outside:
        print(json.dumps({"refused": "roots outside the sandbox: %s" % ", ".join(outside)}))
        return 2
    sys.path.insert(0, str(ROOT / "tools"))
    import toolchain_pack as tp
    calls: list[str] = []
    tp._register_user_path_windows = lambda *a, **k: calls.append("windows")     # the registry
    tp._register_user_path_unix = lambda *a, **k: calls.append("unix")           # the shell profile
    cache = Path(os.environ["RETCOMM_TOOLCHAIN_CACHE"])
    project = sandbox / "project"
    seen: dict[str, object] = {}
    raised = ""
    if case == "heal":
        make_pack(cache / "latest", marker="old.txt")
        before = snapshot(cache)
        tp.heal_broken_toolchain_pointers()
    elif case == "prune":
        make_pack(cache / "1.0.13", "1.0.13", marker="old.txt")
        keep = make_pack(cache / "1.0.14")
        before = snapshot(cache)
        tp.prune_old_toolchain_tags(keep)
    elif case == "install":
        make_pack(cache / "1.0.14", marker="old.txt")
        archive = make_zip(sandbox / "pack.zip")
        before = snapshot(cache)
        try:
            tp.install_from_zip(archive, min_version="0")
        except Exception as exc:    # noqa: BLE001: the text is what is checked
            raised = "%s: %s" % (type(exc).__name__, exc)
    elif case == "pointer":
        pack = make_pack(cache / "1.0.14")
        make_pack(cache / "latest", "1.0.13", marker="old.txt")
        before = snapshot(cache)
        tp._set_latest_pointer(cache, pack)
    elif case == "stamp":
        (project / "toolchain").mkdir(parents=True)
        (project / "toolchain" / tp.STAMP_NAME).write_text("somewhere\n", encoding="utf-8")
        before = snapshot(project)
        tp.clear_project_toolchain_stamp(project)
        seen["stamp_kept"] = (project / "toolchain" / tp.STAMP_NAME).is_file()
        tp.write_toolchain_stamp(sandbox / "project-2", make_pack(cache / "1.0.14") / "bin")
        seen["new_stamp_written"] = (sandbox / "project-2" / "toolchain" / tp.STAMP_NAME).is_file()
        seen["unchanged"] = snapshot(project) == before
        print(json.dumps(seen))
        return 0
    elif case == "user_path":
        pack = make_pack(cache / "1.0.14")
        before = snapshot(cache)
        tp.register_toolchain_user_env(pack)
    elif case == "migrate":
        # The promotion picks its two roots by a folder named retcomm and one named psxrecomp.
        legacy = next(r for r in tp.shared_cache_roots() if "psxrecomp" in r.parts)
        cache = next(r for r in tp.shared_cache_roots() if "retcomm" in r.parts)
        if sandbox not in legacy.parents or sandbox not in cache.parents:
            print(json.dumps({"refused": "a cache root outside the sandbox: %s, %s" % (legacy, cache)}))
            return 2
        make_pack(legacy / "1.0.14")
        before = snapshot(cache)
        tp.migrate_legacy_psxrecomp_cache()
    else:
        return 2
    seen["unchanged"] = snapshot(cache) == before
    seen["raised"] = raised
    seen["registrar_called"] = bool(calls)
    seen["old_marker"] = sorted(str(p.relative_to(cache)) for p in cache.rglob("old.txt"))
    seen["entries"] = sorted(snapshot(cache))
    print(json.dumps(seen))
    return 0


def run_py_case(case: str, switch: bool, tmp: Path) -> dict:
    sandbox = tmp / ("py-%s-%s" % (case, "on" if switch else "off"))
    env = closed_environment(sandbox)
    env["TEST_SANDBOX"] = str(sandbox)
    if switch:
        env[SWITCH] = "1"
    run = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--py-child", case],
                         capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, cwd=str(tmp))
    try:
        seen = json.loads(run.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        seen = {"broken": (run.stdout + run.stderr)[-1500:]}
    seen["stderr"] = run.stderr
    return seen


def python_layer(tmp: Path) -> int:
    failures = 0

    def check(ok: bool, why: str, detail: object = "") -> None:
        nonlocal failures
        if ok:
            print("ok: %s" % why)
        else:
            failures += 1
            print("FAIL: %s\n  %s" % (why, detail))

    for case in PY_CASES:
        on = run_py_case(case, True, tmp)
        off = run_py_case(case, False, tmp)
        if "broken" in on or "broken" in off or "refused" in on or "refused" in off:
            check(False, "CLI %s: the case ran" % case, (on, off))
            continue
        check(on.get("unchanged") is True, "CLI %s, switch set: the cache is byte-for-byte what it was" % case, on)
        check(off.get("unchanged") is False, "CLI %s, control without the switch: the operation changes the cache" % case, off)
        if case != "stamp":
            check("read-only" in on["stderr"], "CLI %s, switch set: it says what it left undone" % case, on["stderr"][-400:])
        if case == "install":
            check("read-only" in on["raised"] and "Nothing was installed" in on["raised"],
                  "CLI install, switch set: it refuses with a reason", on["raised"])
            check(not on["registrar_called"], "CLI install, switch set: the user's login PATH is not written", on)
            check(off["registrar_called"] and not off["old_marker"],
                  "CLI install, control: the installed pack was replaced and the login PATH was written", off)
        if case == "user_path":
            check(not on["registrar_called"] and off["registrar_called"],
                  "CLI user PATH: written without the switch, not written with it", (on, off))
        if case == "stamp":
            check(on["stamp_kept"] and not on["new_stamp_written"],
                  "CLI stamp, switch set: the stamp is neither removed nor written", on)
            check(not off["stamp_kept"] and off["new_stamp_written"],
                  "CLI stamp, control: the stamp is removed and written", off)
    return failures


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
    } else {
        fprintf(stderr, "probe: unknown operation\n");
        return 2;
    }
    printf("ret=%%d\nerr=%%s\n", ret, err);
    return 0;
}
"""

C_CASES = ("heal", "discard_latest", "discard_tag", "prune", "install", "pointer", "stamp", "link", "download")


def find_recomp_ui() -> Path | None:
    env = os.environ.get("RECOMP_UI_ROOT")
    for c in ([Path(env)] if env else []) + [ROOT.parent / "recomp-ui", ROOT / "recomp-ui"]:
        if (c / "src" / "recomp_launcher.h").is_file():
            return c
    return None


def run_c_case(probe: Path, case: str, switch: bool, tmp: Path) -> dict:
    sandbox = tmp / ("c-%s-%s" % (case, "on" if switch else "off"))
    env = closed_environment(sandbox)
    if switch:
        env[SWITCH] = "1"
    outside = roots_outside(env, sandbox)
    if outside:
        return {"refused": "roots outside the sandbox: %s" % ", ".join(outside)}
    cache = Path(env["RETCOMM_TOOLCHAIN_CACHE"])
    project = sandbox / "project"
    project.mkdir()
    env["PROBE_PROJECT"] = str(project)
    watch = cache
    if case == "heal":
        make_pack(cache / "latest", marker="old.txt")
        args = ["heal"]
    elif case == "discard_latest":
        make_pack(cache / "latest", marker="old.txt")
        args = ["discard", str(cache / "latest" / "bin")]
    elif case == "discard_tag":
        make_pack(cache / "1.0.14", marker="old.txt")
        args = ["discard", str(cache / "1.0.14" / "bin")]
    elif case == "prune":
        make_pack(cache / "1.0.13", "1.0.13", marker="old.txt")
        args = ["prune", str(make_pack(cache / "1.0.14"))]
    elif case == "install":
        make_pack(cache / "1.0.14", marker="old.txt")
        args = ["install", str(make_zip(sandbox / "pack.zip"))]
    elif case == "pointer":
        make_pack(cache / "latest", "1.0.13", marker="old.txt")
        args = ["pointer", str(cache), str(make_pack(cache / "1.0.14"))]
    elif case == "stamp":
        (project / "toolchain").mkdir()
        (project / "toolchain" / ".psxrecomp-bin").write_text("somewhere\n", encoding="utf-8")
        args = ["stamp", str(make_pack(cache / "1.0.14") / "bin")]
        watch = project
    elif case == "link":
        args = ["link", str(make_pack(cache / "1.0.14"))]
        watch = project
    elif case == "download":
        make_pack(cache / "1.0.14", marker="old.txt")
        args = ["download"]
    else:
        return {"broken": "unknown case"}
    before = snapshot(watch)
    run = subprocess.run([str(probe)] + args, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         env=env, cwd=str(sandbox))
    lines = dict(line.split("=", 1) for line in run.stdout.splitlines() if "=" in line)
    return {"unchanged": snapshot(watch) == before, "ret": lines.get("ret"), "err": lines.get("err", ""),
            "stderr": run.stderr, "exit": run.returncode, "entries": sorted(snapshot(watch)),
            "others": sorted(str(Path(folder) / p) for folder in sorted(set(SANDBOX_ROOTS.values()))
                             if folder != "retcomm-toolchain-cache" for p in snapshot(sandbox / folder))}


def host_layer(tmp: Path) -> int:
    ui = find_recomp_ui()
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if ui is None or cc is None:
        print("SKIP the setup host layer: %s" % ("recomp-ui not found (set RECOMP_UI_ROOT)" if ui is None
                                                  else "no C compiler on PATH"))
        return 0
    probe_c = tmp / "probe.c"
    probe_c.write_text(PROBE % {"host_c": str(HOST_C).replace("\\", "/")}, encoding="utf-8")
    probe = tmp / ("probe.exe" if WINDOWS else "probe")
    build = subprocess.run(
        [cc, "-std=c11", "-o", str(probe), str(probe_c), '-DPSX_SETUP_BIOS_STEMS="SCPH5552"',
         '-DPSX_SETUP_FRAMEWORK_REL="psxrecomp"', "-I", str(ROOT / "host"), "-I", str(ROOT / "runtime" / "include"),
         "-I", str(ui / "src"), "-I", str(ui / "src" / "common")],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if build.returncode != 0:
        print("FAIL: could not build the host probe\n%s" % build.stderr[-2500:])
        return 1

    failures = 0

    def check(ok: bool, why: str, detail: object = "") -> None:
        nonlocal failures
        if ok:
            print("ok: %s" % why)
        else:
            failures += 1
            print("FAIL: %s\n  %s" % (why, detail))

    for case in C_CASES:
        on = run_c_case(probe, case, True, tmp)
        if "refused" in on or "broken" in on or on["exit"] != 0:
            check(False, "host %s: the case ran" % case, on)
            continue
        check(on["unchanged"], "host %s, switch set: nothing is changed" % case, on)
        check("toolchain read-only" in on["stderr"], "host %s, switch set: it says what it left undone" % case,
              on["stderr"][-400:])
        check(not on["others"], "host %s, switch set: nothing appears in the other folders" % case, on["others"])
        if case == "install":
            check(on["ret"] == "0" and "Nothing was installed" in on["err"],
                  "host install, switch set: it refuses with a reason", on)
        if case == "download":
            check(on["ret"] == "0" and "Nothing was downloaded" in on["err"],
                  "host download, switch set: it refuses before any request", on)
            continue    # no control: without the switch this is a real request to a server
        off = run_c_case(probe, case, False, tmp)
        check(off.get("unchanged") is False, "host %s, control without the switch: the operation is real" % case, off)
    return failures


def main() -> int:
    if "--dry-run" in sys.argv[1:]:
        return dry_run()
    if len(sys.argv) == 3 and sys.argv[1] == "--py-child":
        return py_child(sys.argv[2])
    with tempfile.TemporaryDirectory() as tmp:
        failures = python_layer(Path(tmp))
        if "--cli-only" in sys.argv[1:]:
            print("the setup host layer was not asked for (--cli-only)")
        else:
            failures += host_layer(Path(tmp))
    if failures:
        print("FAILED: %d check(s)" % failures)
        return 1
    print("PASS: with the switch set no toolchain pack or pointer is changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
