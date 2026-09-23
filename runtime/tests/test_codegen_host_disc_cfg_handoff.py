#!/usr/bin/env python3
"""Regression: the setup host hands the recorded disc set to the product.

The wizard writes disc.cfg at the project root and tries build-release/ as
well, but on a fresh kit build-release/ does not exist when the disc is
picked, so that copy fails silently. The runtime reads disc.cfg only beside
its own executable. The first start of the built game therefore asked for the
disc again, while the BIOS was remembered through its project-root fallback
(SF2 0.2.0 Windows replay, 2026-09-23).

sync_project_disc_cfg_to_product() closes that gap on both hand-off routes
(relaunch after Generate, and forward on later starts). This compiles the host
translation unit into a probe and checks the copy rules directly.
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

PROBE = """
#include "psxrecomp_codegen_host.c"
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    snprintf(g_project_root, sizeof(g_project_root), "%s", argv[1]);
    sync_project_disc_cfg_to_product(argv[2]);
    return 0;
}
"""


def find_recomp_ui() -> Path | None:
    env = os.environ.get("RECOMP_UI_ROOT")
    candidates = ([Path(env)] if env else []) + [
        ROOT.parent / "recomp-ui",
        ROOT / "recomp-ui",
    ]
    for c in candidates:
        if (c / "src" / "recomp_launcher.h").is_file():
            return c
    return None


def main() -> int:
    host_text = HOST_C.read_text(encoding="utf-8")
    # Both hand-off routes must call the sync; the forward route had no
    # sidecar handling at all.
    assert host_text.count("sync_project_disc_cfg_to_product(") >= 3, \
        "disc.cfg sync must be defined and called from relaunch and forward"
    ui = find_recomp_ui()
    if ui is None:
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return 0
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler on PATH")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        probe_c = tmp / "probe.c"
        probe_c.write_text(PROBE, encoding="utf-8")
        exe = tmp / ("probe.exe" if os.name == "nt" else "probe")
        build = subprocess.run(
            [cc, "-std=c11", "-o", str(exe), str(probe_c),
             '-DPSX_SETUP_BIOS_STEMS="SCPH1001"',
             '-DPSX_SETUP_FRAMEWORK_REL="psxrecomp"',
             "-I", str(ROOT / "host"),
             "-I", str(ROOT / "runtime" / "include"),
             "-I", str(ui / "src"), "-I", str(ui / "src" / "common")],
            capture_output=True, text=True)
        if build.returncode != 0:
            print("FAIL: could not build probe\n" + build.stderr[-2000:])
            return 1

        two_discs = "C:/Games/SF2 (Disc 1).chd\nC:/Games/SF2 (Disc 2).chd"
        # (project disc.cfg or None, product disc.cfg or None, expected product disc.cfg or None, why)
        cases = [
            (two_discs, None, two_discs + "\n",
             "a missing product disc.cfg receives the whole recorded set"),
            ("C:/Games/one.cue", "", "C:/Games/one.cue\n",
             "an empty product disc.cfg is replaced"),
            (two_discs, "D:/picked in product.cue\n", "D:/picked in product.cue\n",
             "a pick made in the product is never overwritten"),
            (None, None, None,
             "no recorded disc means no product disc.cfg is invented"),
        ]
        failures = 0
        for index, (project, product, expected, why) in enumerate(cases):
            proj = tmp / f"case {index}"
            build_dir = proj / "build-release"
            build_dir.mkdir(parents=True)
            if project is not None:
                (proj / "disc.cfg").write_bytes((project + "\n").encode("utf-8"))
            if product is not None:
                (build_dir / "disc.cfg").write_bytes(product.encode("utf-8"))
            run = subprocess.run([str(exe), str(proj), str(build_dir / "Game.exe")],
                                 capture_output=True, text=True)
            target = build_dir / "disc.cfg"
            got = target.read_text(encoding="utf-8") if target.is_file() else None
            if got is not None:
                got = got.replace("\r\n", "\n")
            if run.returncode != 0 or got != expected:
                print("FAIL: %s\n  expected=%r got=%r rc=%d" % (why, expected, got, run.returncode))
                failures += 1
            else:
                print("ok: %s" % why)
        if failures:
            return 1

    print("PASS: setup host hands the recorded disc set to the product")
    return 0


if __name__ == "__main__":
    sys.exit(main())
