#!/usr/bin/env python3
"""PS1B-146: both Windows build helpers survive replacement of their build tree.

The real host writes each helper. A synthetic CLI runs the current CLI's actual
generator-replacement code; CMake and the final visible launch are stand-ins.
No pack, game, BIOS, network operation or visible console is used.
"""

from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support

ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
#include "%(host_c)s"
int recomp_launcher_relaunch_exe(char* p, size_t n) { (void)p; (void)n; return 0; }
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    PsxrecompCodegenHostConfig cfg = {0};
    cfg.gen_marker_relpath = "generated/probe_dispatch.c";
    g_cfg = &cfg;
    snprintf(g_project_root, sizeof(g_project_root), "%%s", argv[1]);
    snprintf(g_python, sizeof(g_python), "%%s", argv[2]);
    snprintf(g_cli_path, sizeof(g_cli_path), "%%s", argv[3]);
    join_path(g_build_dir, sizeof(g_build_dir), g_project_root, argv[5]);
    join_path(g_game_toml, sizeof(g_game_toml), g_project_root, "game.toml");
    join_path(g_exe_path, sizeof(g_exe_path), g_build_dir, "probe.exe");
    snprintf(g_cmake_target, sizeof(g_cmake_target), "probe");
    snprintf(g_exe_basename, sizeof(g_exe_basename), "probe");
    snprintf(g_display, sizeof(g_display), "synthetic helper control");
    char error[512] = {0};
    if (!write_windows_deferred_rebuild_helper(0, atoi(argv[4]), NULL, error, sizeof(error))) {
        fprintf(stderr, "%%s\n", error);
        return 1;
    }
    printf("%%s\n", g_helper_path);
    return 0;
}
'''

CLI = r'''
import ast, os, re, shutil, sys
from pathlib import Path
from types import SimpleNamespace
from typing import Optional
if sys.argv[1] == "ensure-toolchain":
    raise SystemExit(0)
args = sys.argv[2:]
def value(flag):
    return args[args.index(flag) + 1]
root = Path(value("--project-root"))
diagnostic = "--diagnostic-only" in args
build = Path(value("--diagnostic-dir" if diagnostic else "--build-dir"))
source = Path(os.environ["HELPER_CONTROL_CLI"])
tree = ast.parse(source.read_text(encoding="utf-8"))
functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
             and node.name in ("_read_cmake_cache_generator", "_cmake_configure")]
assert len(functions) == 2
def cmake(command, **kwargs):
    assert command[command.index("-B") + 1] == str(build)
    assert command[command.index("-S") + 1] == str(root)
    return SimpleNamespace(returncode=0, stdout="", stderr="")
namespace = dict(Path=Path, Optional=Optional, ProgressReporter=object, re=re,
                 shutil=shutil, sys=sys, subprocess=SimpleNamespace(run=cmake),
                 resolve_embedded_toolchain_bin=lambda _: None,
                 _tool_in_dir=lambda *_: None,
                 _which_tool=lambda name: "synthetic-" + name if name in ("cmake", "ninja") else None,
                 _pack_sysroot_cmake_args=lambda *_: [])
exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), "exec"), namespace)
namespace["_cmake_configure"](root, build, pgo="", extra=[],
                              progress=SimpleNamespace(log=lambda _: None))
assert not (build / "CMakeCache.txt").exists(), "non-Ninja cache was not replaced"
(build / "configured.txt").write_text("actual CLI replacement reached\n", encoding="utf-8")
if not diagnostic:
    (build / "probe.exe").write_bytes(b"synthetic rebuilt product")
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-source", type=Path, default=support.HOST_C)
    parser.add_argument("--compiler")
    args = parser.parse_args()
    if os.name != "nt":
        print("SKIP: requires Windows cmd.exe and a native Windows C compiler")
        return 0
    compiler = args.compiler or support.find_compiler([])
    with tempfile.TemporaryDirectory(prefix="ps1b146-") as temporary:
        tmp = Path(temporary)
        with patch.object(support, "PROBE", PROBE), patch.object(support, "HOST_C", args.host_source):
            probe, error = support.build_probe(tmp, compiler)
        if probe is None:
            print(error)
            return 0 if error.startswith("SKIP:") else 1
        sandbox = tmp / "sandbox"
        env = support.closed_environment(sandbox)
        env[support.SWITCH] = "1"
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["HELPER_CONTROL_CLI"] = str(ROOT / "psxrecomp_cli.py")
        assert not support.roots_outside(env, sandbox)
        project = sandbox / "project (space)"
        project.mkdir()
        (project / "game.toml").write_text("# synthetic control\n", encoding="utf-8")
        (project / "generated").mkdir()
        (project / "generated/probe_dispatch.c").write_text("/* synthetic marker */\n", encoding="utf-8")
        normal = project / "build-release"
        normal.mkdir()
        product = normal / "probe.exe"
        product.write_bytes(b"synthetic existing product")
        cli = sandbox / "control_cli.py"
        cli.write_text(CLI, encoding="utf-8")
        helpers = []
        for diagnostic in (0, 1):
            result = subprocess.run([str(probe), str(project), sys.executable, str(cli), str(diagnostic), "build-release"],
                                    env=env, cwd=sandbox, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=15)
            assert result.returncode == 0, result.stderr
            helper = Path(result.stdout.strip())
            raw = helper.read_bytes()
            assert (b"--diagnostic-only" in raw) == bool(diagnostic)
            if helpers:
                assert helpers[0][0].read_bytes() == helpers[0][1], "diagnostic overwrote ordinary helper"
            helpers.append((helper, raw))
        failures = 0
        for diagnostic, (helper, raw) in enumerate(helpers):
            build = project / ("build-diagnostic" if diagnostic else "build-release")
            build.mkdir(exist_ok=True)
            (build / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=NMake Makefiles\n", encoding="utf-8")
            before_product = product.read_bytes()
            # Suppress only the final visible launch; leave the actual build command and path intact.
            script = raw.decode("utf-8")
            script = "\r\n".join('echo FIXTURE_CONTINUED> "%ROOT%\\continued.txt"'
                                   if line.startswith('start "" /D ') else line
                                   for line in script.splitlines()) + "\r\n"
            helper.write_bytes(script.encode("utf-8"))
            continued = project / "continued.txt"
            continued.unlink(missing_ok=True)
            command = '"%s" /d /c ""%s""' % (env["ComSpec"], helper)
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
            result = subprocess.run(command, env=env, cwd=project, stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, encoding="utf-8", errors="replace",
                                    timeout=25, startupinfo=startup,
                                    creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            build_trees = (normal, project / "build-diagnostic")
            survived = helper.is_file() and not any(tree == helper or tree in helper.parents for tree in build_trees)
            configured = (build / "configured.txt").is_file()
            unchanged = product.read_bytes() == before_product if diagnostic else None
            passed = configured and survived and continued.is_file() and result.returncode == 0 and unchanged is not False
            failures += not passed
            print(json.dumps(dict(route="diagnostic" if diagnostic else "ordinary", passed=passed,
                                  helper=str(helper), original_helper_sha256=hashlib.sha256(raw).hexdigest(),
                                  executed_helper_sha256=hashlib.sha256(script.encode("utf-8")).hexdigest(),
                                  actual_configure_reached=configured,
                                  helper_survived=survived, continuation=continued.is_file(),
                                  normal_product_preserved=unchanged, exit=result.returncode,
                                  stdout=result.stdout, stderr=result.stderr)))
        # The configured ordinary build tree may be scratch itself. Refuse both
        # routes before writing into it, including equivalent path spellings.
        collision = sandbox / "collision (space)"
        scratch = collision / "_scratch"
        scratch.mkdir(parents=True)
        marker = scratch / "CMakeCache.txt"
        marker.write_bytes(b"CMAKE_GENERATOR:INTERNAL=NMake Makefiles\n")
        before = support.snapshot(collision)
        for build_name in ("_scratch", "_scratch/", "_scratch/../_scratch"):
            for diagnostic in (0, 1):
                result = subprocess.run([str(probe), str(collision), sys.executable, str(cli),
                                         str(diagnostic), build_name],
                                        env=env, cwd=sandbox, capture_output=True, text=True,
                                        encoding="utf-8", errors="replace", timeout=15)
                unchanged = support.snapshot(collision) == before
                passed = result.returncode == 1 and "overlaps" in result.stderr and unchanged
                failures += not passed
                print(json.dumps(dict(route="diagnostic" if diagnostic else "ordinary", passed=passed,
                                      case="scratch-build-overlap", build_dir_name=build_name,
                                      refused_before_write=unchanged, exit=result.returncode,
                                      stdout=result.stdout, stderr=result.stderr)))
        return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
