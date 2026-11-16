#!/usr/bin/env python3
"""Regression (PS1B-684): a first Windows setup hands the BIOS pick to the built game.

On a first install build-release/ does not exist when setup finishes, so the
setup host records bios.cfg and disc.cfg at the project root and beside its
rebuild helper in _scratch/. The helper then builds the game and starts
build-release/<exe>, and the runtime reads bios.cfg only beside its own exe.
Nothing copied the pick there, so the first start asked for the BIOS again
(Alien Resurrection, SCPH-5552, 0.1.23 spot check, 2026-11-15). SCPH-1001
titles hid the fault: the runtime found the framework's own copy,
psxrecomp/bios/SCPH1001.BIN, by its known name.

This compiles the host into a probe that writes the real deferred-rebuild
helper, takes the helper's sidecar hand-over lines, and runs them in cmd with
a built exe in place. Nothing is built and no program is started.
"""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HOST_C = ROOT / "host" / "psxrecomp_codegen_host.c"
BEGIN, END = "rem sidecar-handover-begin", "rem sidecar-handover-end"

PROBE = """
#include "psxrecomp_codegen_host.c"
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
int main(int argc, char** argv) {
    static PsxrecompCodegenHostConfig cfg;
    char err[512];
    if (argc != 2) return 2;
    g_cfg = &cfg;
    snprintf(g_project_root, sizeof(g_project_root), "%s", argv[1]);
    snprintf(g_build_dir, sizeof(g_build_dir), "%s\\\\build-release", argv[1]);
    snprintf(g_exe_path, sizeof(g_exe_path), "%s\\\\Game.exe", g_build_dir);
    snprintf(g_exe_basename, sizeof(g_exe_basename), "Game");
    snprintf(g_cmake_target, sizeof(g_cmake_target), "game");
    snprintf(g_display, sizeof(g_display), "Game");
    if (!write_windows_deferred_rebuild_helper(0, 0, NULL, err, sizeof(err))) {
        fprintf(stderr, "%s\\n", err);
        return 1;
    }
    printf("%s\\n", g_helper_path);
    return 0;
}
"""


def find_recomp_ui() -> Path | None:
    env = os.environ.get("RECOMP_UI_ROOT")
    for c in ([Path(env)] if env else []) + [ROOT.parent / "recomp-ui", ROOT / "recomp-ui"]:
        if (c / "src" / "recomp_launcher.h").is_file():
            return c
    return None


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", **kw)


def main() -> int:
    if os.name != "nt":
        print("SKIP: the deferred rebuild helper is Windows only")
        return 77
    ui = find_recomp_ui()
    if ui is None:
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return 77
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler on PATH")
        return 77

    # Europe (SCPH-5552) and Japan (SCPH-5500) kits: the hand-over must not depend on the BIOS.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        failures = 0
        for stem, name in (("SCPH5552", "[EU] SCPH5552.BIN"), ("SCPH5500", "[JAP] SCPH5500.BIN")):
            (tmp / f"probe-{stem}.c").write_text(PROBE, encoding="utf-8")
            exe = tmp / f"probe-{stem}.exe"
            build = run([cc, "-std=gnu11", "-o", str(exe), str(tmp / f"probe-{stem}.c"),
                         str(ROOT / "runtime/src/start_refusal.c"),
                         f'-DPSX_SETUP_BIOS_STEMS="{stem}"', '-DPSX_SETUP_FRAMEWORK_REL="psxrecomp"',
                         "-I", str(ROOT / "host"), "-I", str(ROOT / "runtime" / "include"),
                         "-I", str(ui / "src"), "-I", str(ui / "src" / "common")])
            if build.returncode != 0:
                print("FAIL: could not build probe\n" + build.stderr[-2000:])
                return 1

            pick = f"Z:/Emulators/PS1 Games/{name}"
            disc = "Z:/Games/Title (Europe).chd"
            # (product bios.cfg before, root bios.cfg, expected product bios.cfg, why)
            cases = [
                (None, pick, pick, "a first install receives the setup BIOS pick"),
                ("D:/picked in product.bin\n", pick, "D:/picked in product.bin",
                 "a pick made in the product is never overwritten"),
                (None, "", "", "an OpenBIOS choice in setup stays OpenBIOS"),
                (None, None, None, "no setup pick means no product bios.cfg is invented"),
            ]
            for index, (product, root_bios, expected, why) in enumerate(cases):
                proj = tmp / f"Install (1) {stem} {index}"
                proj.mkdir()
                got = run([str(exe), str(proj)])
                helper = Path(got.stdout.strip())
                if got.returncode != 0 or not helper.is_file():
                    print("FAIL: probe could not write the helper: " + got.stderr[-500:])
                    return 1
                lines = helper.read_text(encoding="utf-8").splitlines()
                if BEGIN not in lines or END not in lines:
                    print("FAIL: %s\n  the helper has no sidecar hand-over to the built game" % why)
                    failures += 1
                    continue
                build_dir = proj / "build-release"
                build_dir.mkdir(exist_ok=True)
                (build_dir / "Game.exe").write_bytes(b"")
                if root_bios is not None:
                    (proj / "bios.cfg").write_text(root_bios + "\n", encoding="utf-8")
                    (proj / "disc.cfg").write_text(disc + "\n", encoding="utf-8")
                if product is not None:
                    (build_dir / "bios.cfg").write_text(product, encoding="utf-8")
                part = lines[lines.index(BEGIN):lines.index(END) + 1]
                script = proj / "_scratch" / "handover-only.cmd"
                script.parent.mkdir(exist_ok=True)
                script.write_text("\r\n".join(["@echo off", "setlocal EnableExtensions",
                                               f'set "ROOT={proj}"',
                                               f'set "EXE_FINAL={build_dir / "Game.exe"}"',
                                               *part, "exit /b 0", ""]), encoding="utf-8")
                res = run(["cmd.exe", "/d", "/c", str(script)])
                target = build_dir / "bios.cfg"
                got_bios = target.read_text(encoding="utf-8").strip() if target.is_file() else None
                disc_ok = root_bios is None or (build_dir / "disc.cfg").is_file()
                if res.returncode != 0 or got_bios != expected or not disc_ok:
                    print("FAIL: %s\n  expected=%r got=%r disc.cfg=%s rc=%d %s"
                          % (why, expected, got_bios, disc_ok, res.returncode, res.stdout + res.stderr))
                    failures += 1
                else:
                    print(f"ok: {stem}: {why}")
        if failures:
            return 1
    print("PASS: the deferred rebuild hands the setup BIOS pick to the built game")
    return 0


if __name__ == "__main__":
    sys.exit(main())
