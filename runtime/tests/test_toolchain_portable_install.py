#!/usr/bin/env python3
"""Where a fresh toolchain install goes (PS1B-687).

A setup program is portable: it keeps what it installs in the game folder.
The setup host (host/psxrecomp_codegen_host.c) and the CLI (tools/toolchain_pack.py)
used to install the cmake-clang-v1 pack into %LOCALAPPDATA%\\retcomm\\toolchains
(XDG_DATA_HOME or ~/.local/share elsewhere) and download it through %TEMP%. The
rules now, each checked for the CLI's module and for the host (a probe that
includes its source file):

  a fresh install goes to the game   with no pack anywhere and no toolchain
                                     variable, a download lands in
                                     <project>/toolchain/<tag>, with the pointer
                                     <project>/toolchain/latest beside it.
  the download stays in the game     the zip is written inside
                                     <project>/toolchain and removed after use;
                                     nothing is written under LOCALAPPDATA,
                                     APPDATA, the home folder, XDG_DATA_HOME or
                                     TEMP.
  an older install is still found    a pack under the legacy LOCALAPPDATA (or
                                     XDG) cache is found when the project has
                                     none, and nothing is written there.
  the login environment stays    the CLI writes the user's login PATH and
                                     RETCOMM_TOOLCHAIN_DIR (registry / shell
                                     profile; the writers are intercepted) only
                                     with --persist-env; by default it sets them
                                     for its own process and children.
  an override comes first           RETCOMM_TOOLCHAIN_CACHE holds a pack and so
                                     does the project: the override is used.

Everything runs in the closed environment of toolchain_test_support.py, with
RETCOMM_TOOLCHAIN_CACHE and RETCOMM_DATA_HOME removed except in the override
case. No network: the CLI's download function and the host's curl are stand-ins
that copy a made-up zip. --dry-run prints the environment and starts nothing.
--cli-only leaves the host layer out. A pack that passes its check needs a
program that runs (--compiler, CC or the PATH on Windows); the host layer also
needs recomp-ui (RECOMP_UI_ROOT).
"""

from __future__ import annotations

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402
from toolchain_test_support import CMAKE, EXE, WINDOWS, make_pack, make_zip, snapshot  # noqa: E402

THIS = Path(__file__).resolve()
TAG = "1.0.14"
OVERRIDES = ("RETCOMM_TOOLCHAIN_CACHE", "RETCOMM_DATA_HOME")
# The roots a setup program must not write into by default (closed-root folders of the sandbox).
FOREIGN = ("localappdata", "appdata", "home", "xdg-data", "temp")
LEGACY = Path("localappdata") / "retcomm" / "toolchains" / "cmake-clang-v1" if WINDOWS else \
    Path("xdg-data") / "retcomm" / "toolchains" / "cmake-clang-v1"


def environment(sandbox: Path, keep_overrides: bool = False) -> dict:
    env = support.closed_environment(sandbox)
    if not keep_overrides:
        for name in OVERRIDES:
            env.pop(name, None)
    (sandbox / "project").mkdir(exist_ok=True)
    return env


def foreign_entries(sandbox: Path, before: dict | None = None) -> list[str]:
    """What appeared (or changed) under the roots a setup program must not write into."""
    found = []
    for folder in FOREIGN:
        now = snapshot(sandbox / folder)
        was = (before or {}).get(folder, {})
        found += [os.path.join(folder, k) for k, v in now.items() if was.get(k) != v]
    return sorted(found)


def foreign_snapshot(sandbox: Path) -> dict:
    return {folder: snapshot(sandbox / folder) for folder in FOREIGN}


def under(path: str, parent: Path) -> bool:
    try:
        return Path(path).resolve().is_relative_to(parent.resolve())
    except (OSError, ValueError):
        return False


# --- the CLI's module ------------------------------------------------------------------------------

def cli_child(case: str) -> int:
    calls: list[str] = []
    sys.path.insert(0, str(support.ROOT / "tools"))
    import toolchain_pack as tp
    tp._register_user_path_windows = lambda *a, **k: calls.append("windows")
    tp._register_user_path_unix = lambda *a, **k: calls.append("unix")
    sandbox = Path(os.environ["TEST_SANDBOX"])
    project = sandbox / "project"
    out: dict = {}
    if case in ("fresh", "persist"):
        written: list[str] = []

        def stand_in(url, dest, token=None):       # no network: the made-up zip is the download
            written.append(str(dest))
            Path(dest).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(sandbox / "pack.zip", dest)
        tp.download_url = stand_in
        try:
            # "persist" is the --persist-env opt-in (ensure_toolchain(persist_env=True) sets it).
            # download_latest_pack, not ensure_toolchain: on Linux and macOS that uses native tools.
            tp.set_persist_env(case == "persist")
            root = tp.download_latest_pack(project_root=project, min_version="0")
            out["root"] = str(root)
        except Exception as exc:    # noqa: BLE001: the text is what is reported
            out["raised"] = "%s: %s" % (type(exc).__name__, exc)
        out["written"] = written
        out["registrar_calls"] = calls      # the registry / shell-profile writers, intercepted
        out["session_dir"] = os.environ.get("RETCOMM_TOOLCHAIN_DIR", "")
    else:   # legacy / override: what the lookup finds
        found = tp.resolve_toolchain_bin(project, min_version="0")
        out["bin"] = str(found) if found else ""
    print(json.dumps(out))
    return 0


def run_cli(case: str, sandbox: Path, keep_overrides: bool = False) -> dict:
    env = environment(sandbox, keep_overrides)
    env["TEST_SANDBOX"] = str(sandbox)
    run = subprocess.run([sys.executable, "-B", str(THIS), "--cli-child", case], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", env=env, cwd=str(sandbox))
    try:
        seen = json.loads(run.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        seen = {"broken": (run.stdout + run.stderr)[-1500:]}
    seen["stderr"] = run.stderr
    return seen


# --- the setup host --------------------------------------------------------------------------------

CURL_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* curl stand-in: copies STUB_ZIP to the file after -o and appends that path to STUB_LOG. */
int main(int argc, char** argv) {
    const char* src = getenv("STUB_ZIP");
    const char* log = getenv("STUB_LOG");
    const char* dest = NULL;
    char buf[65536];
    size_t n;
    FILE *in, *out;
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], "-o") == 0)
            dest = argv[i + 1];
    if (!src || !dest)
        return 2;
    if (log && (out = fopen(log, "a")) != NULL) {
        fprintf(out, "%s\n", dest);
        fclose(out);
    }
    in = fopen(src, "rb");
    out = fopen(dest, "wb");
    if (!in || !out)
        return 23;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 0;
}
"""


def make_curl(probe: Path, cc: str) -> Path | None:
    """A curl the host finds before the system's: beside the probe on Windows (CreateProcess
    looks there first), first on PATH elsewhere."""
    if not WINDOWS:
        folder = probe.parent / "curl-bin"
        folder.mkdir(exist_ok=True)
        curl = folder / "curl"
        curl.write_text('#!/bin/sh\nout=\nwhile [ $# -gt 0 ]; do [ "$1" = -o ] && out="$2"; shift; done\n'
                        'echo "$out" >> "$STUB_LOG"\ncp "$STUB_ZIP" "$out"\n', encoding="utf-8")
        curl.chmod(0o755)
        return curl
    source = probe.parent / "curl-stub.c"
    source.write_text(CURL_C, encoding="utf-8")
    curl = probe.parent / "curl.exe"
    build = subprocess.run([cc, "-o", str(curl), str(source)], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    return curl if build.returncode == 0 and curl.is_file() else None


def run_host(probe: Path, curl: Path | None, args: list[str], sandbox: Path, keep_overrides: bool = False) -> dict:
    env = environment(sandbox, keep_overrides)
    env["PROBE_PROJECT"] = str(sandbox / "project")
    env["STUB_ZIP"] = str(sandbox / "pack.zip")
    env["STUB_LOG"] = str(sandbox / "curl.log")
    if curl is not None and not WINDOWS:
        env["PATH"] = str(curl.parent) + os.pathsep + env["PATH"]
    run = subprocess.run([str(probe)] + args, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         env=env, cwd=str(sandbox))
    seen = dict(line.split("=", 1) for line in run.stdout.splitlines() if "=" in line)
    seen.update({"stderr": run.stderr, "exit": run.returncode})
    log = sandbox / "curl.log"
    seen["written"] = log.read_text(encoding="utf-8").split() if log.is_file() else []
    return seen


# --- the cases -------------------------------------------------------------------------------------

def fresh_case(name: str, run, tmp: Path, stub: Path, check: support.Checks) -> None:
    sandbox = tmp / (name + "-fresh")
    environment(sandbox)
    make_zip(sandbox / "pack.zip", version=TAG, stub=stub)
    before = foreign_snapshot(sandbox)
    seen = run(sandbox)
    project_tc = sandbox / "project" / "toolchain"
    installed = project_tc / TAG / "bin" / CMAKE
    check(installed.is_file(), "%s fresh: with no pack anywhere the download lands in <project>/toolchain/%s"
          % (name, TAG), {k: v for k, v in seen.items() if k != "stderr"} | {"stderr": seen.get("stderr", "")[-800:],
                                                                             "project": sorted(snapshot(sandbox / "project"))[:20],
                                                                             "foreign": foreign_entries(sandbox, before)[:20]})
    check((project_tc / "latest" / "bin" / CMAKE).is_file(),
          "%s fresh: the latest pointer is in <project>/toolchain" % name, sorted(snapshot(project_tc))[:20])
    written = seen.get("written", [])
    check(bool(written) and all(under(w, project_tc) for w in written),
          "%s fresh: the zip was downloaded inside <project>/toolchain, not the temporary folder" % name, written)
    left = [n for n in (sorted(p.name for p in project_tc.iterdir()) if project_tc.is_dir() else [])
            if n.startswith(".") and n != ".psxrecomp-bin"]
    check(not left, "%s fresh: the download and staging folders are removed after use" % name, left)
    foreign = foreign_entries(sandbox, before)
    check(not foreign, "%s fresh: nothing is written under LOCALAPPDATA, APPDATA, home, XDG_DATA_HOME or TEMP"
          % name, foreign[:20])
    if "registrar_calls" in seen:   # the CLI: the user's login environment (registry / shell profile)
        check(seen["registrar_calls"] == [],
              "%s fresh: the user's login PATH and RETCOMM_TOOLCHAIN_DIR are not written by default" % name,
              seen["registrar_calls"])
        check(under(seen.get("session_dir", ""), project_tc),
              "%s fresh: RETCOMM_TOOLCHAIN_DIR is set for this process and its children only" % name,
              seen.get("session_dir"))


def persist_case(tmp: Path, stub: Path, check: support.Checks) -> None:
    """The control: with the --persist-env opt-in the CLI does call the login-environment writer."""
    sandbox = tmp / "CLI-persist"
    environment(sandbox)
    make_zip(sandbox / "pack.zip", version=TAG, stub=stub)
    seen = run_cli("persist", sandbox)
    check(seen.get("registrar_calls") == ["windows" if WINDOWS else "unix"],
          "CLI persist (control): with --persist-env the login environment writer is called once",
          {k: v for k, v in seen.items() if k != "stderr"})


def legacy_case(name: str, run, tmp: Path, stub: Path, check: support.Checks) -> None:
    sandbox = tmp / (name + "-legacy")
    environment(sandbox)
    pack = make_pack(sandbox / LEGACY / TAG, version=TAG, stub=stub)
    before = foreign_snapshot(sandbox)
    seen = run(sandbox)
    found = seen.get("bin") or seen.get("first", "")
    check(found and Path(found).resolve() == (pack / "bin").resolve(),
          "%s legacy: a pack in the legacy cache is found when the project has none" % name,
          {"found": found, "stderr": seen.get("stderr", "")[-600:]})
    foreign = foreign_entries(sandbox, before)
    check(not foreign, "%s legacy: the legacy cache is read, nothing is written there" % name, foreign[:20])


def override_case(name: str, run, tmp: Path, stub: Path, check: support.Checks) -> None:
    sandbox = tmp / (name + "-override")
    environment(sandbox, keep_overrides=True)
    wanted = make_pack(sandbox / "retcomm-toolchain-cache" / TAG, version=TAG, stub=stub)
    make_pack(sandbox / "project" / "toolchain" / TAG, version=TAG, stub=stub)
    seen = run(sandbox)
    found = seen.get("bin") or seen.get("first", "")
    check(found and Path(found).resolve() == (wanted / "bin").resolve(),
          "%s override: RETCOMM_TOOLCHAIN_CACHE comes before the project's toolchain folder" % name,
          {"found": found, "stderr": seen.get("stderr", "")[-600:]})


def main() -> int:
    argv = sys.argv[1:]
    if "--dry-run" in argv:
        return support.dry_run()
    if len(argv) == 2 and argv[0] == "--cli-child":
        return cli_child(argv[1])
    check = support.Checks()
    cc = support.find_compiler(argv)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stub = support.make_stub(tmp, cc)
        if stub is None:
            print("SKIP: no program that runs can be made here (no C compiler)")
            return 77
        fresh_case("CLI", lambda sb: run_cli("fresh", sb), tmp, stub, check)
        persist_case(tmp, stub, check)
        legacy_case("CLI", lambda sb: run_cli("legacy", sb), tmp, stub, check)
        override_case("CLI", lambda sb: run_cli("override", sb, keep_overrides=True), tmp, stub, check)
        if "--cli-only" in argv:
            print("the setup host layer was not asked for (--cli-only)")
        else:
            probe, why = support.build_probe(tmp, cc)
            if probe is None:
                print(why if why.startswith("FAIL") else "%s; the setup host layer did not run" % why)
                check.failures += 1 if why.startswith("FAIL") else 0
            else:
                curl = make_curl(probe, cc)
                if curl is None:
                    check(False, "host: the curl stand-in could be built")
                else:
                    fresh_case("host", lambda sb: run_host(probe, curl, ["download"], sb), tmp, stub, check)
                    legacy_case("host", lambda sb: run_host(probe, curl, ["resolve"], sb), tmp, stub, check)
                    override_case("host", lambda sb: run_host(probe, curl, ["resolve"], sb, keep_overrides=True),
                                  tmp, stub, check)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: a fresh toolchain install lands in the game folder and nothing is written outside it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
