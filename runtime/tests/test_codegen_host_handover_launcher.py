#!/usr/bin/env python3
"""The setup host's hand-over to the product honours a headless start.

A kit's own exe is the setup host. Once the game is built it starts the product
in build-release and exits. It added --launcher to that start, always, so that
the product's persisted skip_launcher setting cannot hide the launcher on a
hand-over. In the product --launcher wins over --no-launcher, --headless,
PSX_NO_LAUNCHER and PSX_HEADLESS. So a scripted start of a kit's own exe always
waited in the launcher, and a gate that starts the kit the way a player does
could not get a frame from the game it hands over to (PS1B-402: no gate made
that start, and six public kits that never handed over passed).

This builds the real host translation unit into a stand-in setup program, puts
a stand-in product where the host looks for it, and reads the arguments the
product was started with:

  a player's start      --launcher is added, once, after the caller's arguments;
  a headless start      --no-launcher or --headless on the command line, or
                        PSX_NO_LAUNCHER or PSX_HEADLESS set to something other
                        than empty or 0: the arguments are forwarded as they
                        are, with no --launcher;
  a variable set to 0   is not a headless start.
"""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
HOST_C = ROOT / "host" / "psxrecomp_codegen_host.c"

SETUP_HOST = """
#include <string.h>
#include <stdio.h>
#include "psxrecomp_codegen_host.h"
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
int main(int argc, char** argv) {
    PsxrecompCodegenHostConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmake_target       = "psx-runtime";
    cfg.exe_basename       = "Probe";
    cfg.gen_marker_relpath = "generated/SCUS_943.51_dispatch.c";
    psxrecomp_codegen_host_forward_if_built(&cfg, argc, argv);
    puts("NOT FORWARDED");   /* the hand-over replaces or ends this process */
    return 3;
}
"""

# The product: it writes the arguments it was started with, one per line.
PRODUCT = """
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    const char* out = getenv("HANDOVER_ARGS_OUT");
    FILE* f = out ? fopen(out, "wb") : NULL;
    int i;
    if (!f) return 4;
    fprintf(f, "started\\n");
    for (i = 1; i < argc; ++i) fprintf(f, "%s\\n", argv[i]);
    fclose(f);
    return 0;
}
"""

# Variables that change what the host does; the test sets the ones a case names.
SIGNALS = ("PSX_NO_LAUNCHER", "PSX_HEADLESS", "PSXRECOMP_NO_FORWARD", "PSXRECOMP_FORCE_SETUP",
           "PSXRECOMP_DIAGNOSTIC", "PSXRECOMP_BUILD_DIR", "PSXRECOMP_PROJECT_ROOT")


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


def make_project(root: Path, framework: str) -> None:
    """A built project as the host sees it: sources generated, a product folder."""
    (root / "generated").mkdir(parents=True)
    (root / "generated" / "SCUS_943.51_dispatch.c").write_text("", encoding="utf-8")
    fw_gen = root / framework / "generated"
    fw_gen.mkdir(parents=True)
    (fw_gen / "SCPH5552_dispatch.c").write_text(
        "const PsxBiosBackend SCPH5552_psx_bios_backend = {};\n", encoding="utf-8")
    (fw_gen / "SCPH5552_full.c").write_text("", encoding="utf-8")
    (root / "game.toml").write_text("[game]\n", encoding="utf-8")
    (root / "psxrecomp").mkdir(exist_ok=True)
    (root / "psxrecomp" / "psxrecomp_cli.py").write_text("", encoding="utf-8")
    (root / "build-release").mkdir()


def main() -> int:
    ui = find_recomp_ui()
    if ui is None:
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return 0
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler on PATH")
        return 0
    suffix = ".exe" if os.name == "nt" else ""
    framework = "psxrecomp"

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "host.c").write_text(SETUP_HOST, encoding="utf-8")
        (tmp / "product.c").write_text(PRODUCT, encoding="utf-8")
        host_exe = tmp / ("setup-host" + suffix)
        product_exe = tmp / ("product" + suffix)
        for what, argv in (
            ("the setup host", [cc, "-std=c11", "-o", str(host_exe), str(tmp / "host.c"), str(HOST_C),
                                '-DPSX_SETUP_BIOS_STEMS="SCPH5552"', f'-DPSX_SETUP_FRAMEWORK_REL="{framework}"',
                                "-I", str(ROOT / "host"), "-I", str(ROOT / "runtime" / "include"),
                                "-I", str(ui / "src"), "-I", str(ui / "src" / "common")]),
            ("the product", [cc, "-std=c11", "-o", str(product_exe), str(tmp / "product.c")]),
        ):
            build = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
            if build.returncode != 0:
                print("FAIL: could not build %s\n%s" % (what, build.stderr[-2000:]))
                return 1

        launcher = ["--launcher"]
        # (arguments, variables, what the product must get, why)
        cases = [
            ([], {}, launcher,
             "a player's start: the launcher is forced"),
            (["--bios", "b i o s.bin"], {}, ["--bios", "b i o s.bin"] + launcher,
             "a player's start with arguments: they are forwarded and --launcher follows"),
            (["--launcher"], {}, launcher,
             "--launcher given by the caller is not doubled"),
            ([], {"PSX_HEADLESS": "0", "PSX_NO_LAUNCHER": "0"}, launcher,
             "a variable set to 0 is not a headless start"),
            ([], {"PSX_HEADLESS": "1"}, [],
             "PSX_HEADLESS=1: no --launcher"),
            ([], {"PSX_NO_LAUNCHER": "1"}, [],
             "PSX_NO_LAUNCHER=1: no --launcher"),
            (["--no-launcher"], {}, ["--no-launcher"],
             "--no-launcher is forwarded as it is"),
            (["--headless", "--disc", "d.cue"], {}, ["--headless", "--disc", "d.cue"],
             "--headless is forwarded as it is"),
            (["--bios", "b.bin"], {"PSX_HEADLESS": "1", "PSX_NO_LAUNCHER": "1"}, ["--bios", "b.bin"],
             "the fleet's start (both variables): arguments only"),
        ]
        failures = 0
        for index, (args, variables, expected, why) in enumerate(cases):
            project = tmp / f"case {index}"
            make_project(project, framework)
            shutil.copy2(product_exe, project / "build-release" / ("Probe" + suffix))
            out = project / "handover-args.txt"
            env = {k: v for k, v in os.environ.items() if k not in SIGNALS}
            env.update(variables)
            env["PSXRECOMP_PROJECT_ROOT"] = str(project)
            env["HANDOVER_ARGS_OUT"] = str(out)
            run = subprocess.run([str(host_exe)] + args, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", env=env, cwd=str(tmp))
            # On Windows the host starts the product and exits without waiting for it.
            deadline = time.time() + 15.0
            got = None
            while time.time() < deadline:
                if out.is_file():
                    lines = out.read_text(encoding="utf-8", errors="replace").splitlines()
                    if lines and lines[0] == "started":
                        got = lines[1:]
                        break
                time.sleep(0.05)
            if "NOT FORWARDED" in run.stdout:
                print("FAIL: %s\n  the host did not hand over: %s" % (why, run.stderr.strip()[-400:]))
                failures += 1
            elif got != expected:
                print("FAIL: %s\n  expected %r, the product got %r" % (why, expected, got))
                failures += 1
            else:
                print("ok: %s" % why)
        if failures:
            return 1

    print("PASS: the hand-over forces the launcher for a player and honours a headless start")
    return 0


if __name__ == "__main__":
    sys.exit(main())
