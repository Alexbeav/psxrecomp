#!/usr/bin/env python3
"""An on-demand diagnostic build says whether overlay_toolchain/ is beside it (PS1B-153).

A set-up kit builds its diagnostic product on first request: the setup host,
started with --diagnostic and no product in build-diagnostic/, runs the CLI's
`rebuild --diagnostic-only` (host_build_diagnostic_on_demand). The CLI stages
overlay_toolchain/ beside the product; when staging fails it logs a WARNING and
the product runs overlays interpreted. On Rocky 9 (2026-09) staging failed that
way, and nothing showed it: on this route the host ran the CLI with no progress
callback, so every line the CLI wrote was dropped.

This builds the real host translation unit into a probe that calls
host_build_diagnostic_on_demand, with a stand-in CLI in the project. The
stand-in checks that it was asked for the diagnostic product alone, makes the
product, and then either stages overlay_toolchain/ or writes the CLI's own
WARNING for a failed staging. The project folder is long, so the CLI's
"staged" line runs past 240 characters. Checks:

  staged     stderr carries the CLI's "overlay toolchain staged at <path>" line
             whole, and the host's own line that overlay_toolchain/ is beside
             the product;
  failed     stderr carries the CLI's WARNING, and the host's WARNING that no
             overlay_toolchain/ is beside the product and overlays will run
             interpreted; the build still succeeds;
  both       the normal product is untouched and nothing was installed.

The host is a program with a toolchain installer in it, so it runs in the
closed environment of toolchain_test_support.py, with stand-ins for cmake,
ninja and the compilers first on PATH: the host's check of the native tools
passes and it installs nothing. POSIX only (on Windows the build is deferred to
a helper and this route prints nothing itself): there it says SKIP. Without
recomp-ui (RECOMP_UI_ROOT) or a C compiler it says SKIP. --dry-run prints the
environment and starts nothing.
"""

from __future__ import annotations

from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402

ROOT = support.ROOT
HOST_C = support.HOST_C
LONG = "project-" + "d" * 200      # a project folder that makes the CLI's lines long

PROBE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "%(host_c)s"
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
/* What psxrecomp_codegen_host_forward_if_built sets before the on-demand build. */
int main(void) {
    static PsxrecompCodegenHostConfig cfg;
    char built[1300], err[512];
    const char* project = getenv("PROBE_PROJECT");
    int ret;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmake_target = "psx-runtime";
    cfg.exe_basename = "Probe";
    g_cfg = &cfg;
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%%s", cfg.cmake_target);
    snprintf(g_exe_basename, sizeof(g_exe_basename), "%%s", cfg.exe_basename);
    snprintf(g_project_root, sizeof(g_project_root), "%%s", project ? project : "");
    if (!resolve_build_paths()) {
        printf("ret=-1\n");
        return 0;
    }
    built[0] = err[0] = '\0';
    ret = host_build_diagnostic_on_demand(built, sizeof(built), err, sizeof(err));
    printf("ret=%%d\nbuilt=%%s\nerr=%%s\n", ret, built, err);
    return 0;
}
"""

# The stand-in CLI. It writes what it was asked to args.json and answers in JSON lines written
# the way tools/sdk_progress.py writes them (sorted keys, no spaces), which the host parses.
CLI = r'''
import json, os, sys
from pathlib import Path
argv = sys.argv[1:]
Path(os.environ["STUB_ARGS_OUT"]).write_text(json.dumps(argv), encoding="utf-8")
def say(**row):
    print(json.dumps(row, sort_keys=True, separators=(",", ":")), flush=True)
if "--diagnostic-only" not in argv or "--diagnostic-dir" not in argv:
    say(event="error", message="the stand-in CLI was not asked for the diagnostic product alone")
    sys.exit(2)
diag = Path(argv[argv.index("--diagnostic-dir") + 1])
diag.mkdir(parents=True, exist_ok=True)
exe = diag / "Probe"
exe.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
exe.chmod(0o755)
say(event="phase", phase="diagnostic", pct=0.9, message="cmake diagnostic build (PSX_DEBUG_TOOLS=ON)...")
tk = diag / "overlay_toolchain"
if os.environ.get("STUB_STAGE") == "1":
    tk.mkdir()
    say(event="log", message="overlay toolchain staged at %s; shard compiler: /usr/bin/cc" % tk)
else:
    say(event="log", message="WARNING: overlay toolchain staging failed; overlays will run interpreted: "
        "unsupported operand type(s) for |: 'type' and 'NoneType'")
say(event="log", message="diagnostic product ready: %s" % exe)
say(event="phase", phase="done", pct=1.0, message="Diagnostic build complete")
say(event="result", ok=True, diagnostic_only=True, diagnostic_exe=str(exe),
    overlay_toolchain=str(tk) if tk.is_dir() else None)
'''

TOOLS = ("cmake", "ninja", "cc", "c++")


def build_probe(tmp: Path, cc: str | None) -> tuple[Path | None, str]:
    ui = support.find_recomp_ui()
    if ui is None:
        return None, "SKIP: recomp-ui not found (set RECOMP_UI_ROOT)"
    if cc is None:
        return None, "SKIP: no C compiler"
    probe_c = tmp / "probe.c"
    probe_c.write_text(PROBE % {"host_c": str(HOST_C)}, encoding="utf-8")
    probe = tmp / "probe"
    build = subprocess.run(
        [cc, "-std=gnu11", "-o", str(probe), str(probe_c), '-DPSX_SETUP_BIOS_STEMS="SCPH5552"',
         '-DPSX_SETUP_FRAMEWORK_REL="psxrecomp"', "-I", str(ROOT / "host"), "-I", str(ROOT / "runtime" / "include"),
         "-I", str(ui / "src"), "-I", str(ui / "src" / "common"), str(ROOT / "runtime/src/start_refusal.c")],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if build.returncode != 0:
        return None, "FAIL: could not build the host probe\n%s" % build.stderr[-2500:]
    return probe, ""


def make_project(sandbox: Path) -> Path:
    """A set-up kit: game.toml, the CLI, and the normal product in build-release/."""
    project = sandbox / LONG
    (project / "psxrecomp").mkdir(parents=True)
    (project / "game.toml").write_text('[game]\nname = "probe"\n', encoding="utf-8")
    (project / "psxrecomp" / "psxrecomp_cli.py").write_text(CLI, encoding="utf-8")
    (project / "build-release").mkdir()
    (project / "build-release" / "Probe").write_text("normal product\n", encoding="utf-8")
    return project


def environment(sandbox: Path, project: Path, staged: bool) -> dict[str, str]:
    env = support.closed_environment(sandbox)
    tools = sandbox / "tools"
    tools.mkdir()
    for name in TOOLS:
        (tools / name).write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        (tools / name).chmod(0o755)
    (tools / "python3").symlink_to(sys.executable)
    env["PATH"] = "%s:%s" % (tools, env["PATH"])
    env["RETCOMM_PYTHON"] = sys.executable
    env["PROBE_PROJECT"] = str(project)
    env["STUB_STAGE"] = "1" if staged else "0"
    env["STUB_ARGS_OUT"] = str(sandbox / "args.json")
    return env


def run_case(probe: Path, tmp: Path, staged: bool, check: support.Checks) -> None:
    name = "staged" if staged else "staging failed"
    sandbox = tmp / ("sandbox-%s" % ("staged" if staged else "failed"))
    project = make_project(sandbox)
    env = environment(sandbox, project, staged)
    outside = support.roots_outside(env, sandbox)
    if not check(not outside, "%s: every toolchain and data root is inside the test's folder" % name, outside):
        return
    run = subprocess.run([str(probe)], capture_output=True, text=True, encoding="utf-8", errors="replace",
                         env=env, cwd=str(sandbox), timeout=120)
    said = dict(line.split("=", 1) for line in run.stdout.splitlines() if "=" in line)
    err = run.stderr
    diag = project / "build-diagnostic"
    tk = diag / "overlay_toolchain"
    if not check(run.returncode == 0 and said.get("ret") == "1",
                 "%s: the on-demand build succeeds" % name, (run.returncode, said, err[-1500:])):
        return
    check(os.path.normpath(said.get("built", "")) == os.path.normpath(str(diag / "Probe")),
          "%s: it returns the diagnostic product" % name, said.get("built"))
    asked = json.loads((sandbox / "args.json").read_text(encoding="utf-8"))
    check("--diagnostic-only" in asked and asked[asked.index("--diagnostic-dir") + 1] == str(diag),
          "%s: the CLI was asked for the diagnostic product alone, in build-diagnostic/" % name, asked)
    prefix = "psxrecomp-codegen: diagnostic build: "
    if staged:
        line = "%soverlay toolchain staged at %s; shard compiler: /usr/bin/cc" % (prefix, tk)
        check(len(line) > 240 + len(prefix) and line in err.splitlines(),
              "%s: stderr carries the CLI's staged line whole (%d characters)" % (name, len(line) - len(prefix)),
              err[-1500:])
        check("%soverlay_toolchain/ is beside the product: %s" % (prefix, tk) in err.splitlines(),
              "%s: the host says overlay_toolchain/ is beside the product" % name, err[-1500:])
        check("WARNING" not in err, "%s: no warning" % name, err[-1500:])
    else:
        check(any(l.startswith(prefix + "WARNING: overlay toolchain staging failed; overlays will run "
                               "interpreted: unsupported operand") for l in err.splitlines()),
              "%s: stderr carries the CLI's WARNING with its cause" % name, err[-1500:])
        check(any(l.startswith(prefix + "WARNING: no overlay_toolchain/ beside %s;" % (diag / "Probe"))
                  and "overlays will run interpreted" in l for l in err.splitlines()),
              "%s: the host warns that no overlay_toolchain/ is beside the product" % name, err[-1500:])
    check((project / "build-release" / "Probe").read_text(encoding="utf-8") == "normal product\n"
          and sorted(p.name for p in (project / "build-release").iterdir()) == ["Probe"],
          "%s: the normal product is untouched" % name, sorted(p.name for p in (project / "build-release").iterdir()))
    left = support.other_roots_entries(sandbox) + sorted(
        str(p) for p in (sandbox / "retcomm-toolchain-cache").rglob("*"))
    check(not left, "%s: nothing was installed" % name, left)


def main() -> int:
    argv = sys.argv[1:]
    if "--dry-run" in argv:
        return support.dry_run()
    if support.WINDOWS:
        print("SKIP: on Windows the on-demand build is deferred to a helper; this test is the POSIX route")
        return 0
    check = support.Checks()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        probe, why = build_probe(tmp, support.find_compiler(argv))
        if probe is None:
            print(why)
            return 1 if why.startswith("FAIL") else 0
        for staged in (True, False):
            run_case(probe, tmp, staged, check)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: an on-demand diagnostic build says whether overlay_toolchain/ is beside it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
