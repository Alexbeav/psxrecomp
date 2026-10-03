#!/usr/bin/env python3
"""package_setup_host.sh --set: a set of programs as one setup package (PS1B-333).

The packager needs a built host to run to the end, so this test drives it only
as far as its own decisions about the set: which set file it accepts, which
source trees it refuses, and that a valid set gets past them. The staging rules
that follow are checked in the script's text. A single-program package must
not change: the lines it depends on are asserted too.

With --zip-part it runs only the packager's last step, the zip, under a shell
that has a zip tool, and exits 77 with a SKIP line when no shell here has one.
ctest registers that as package_setup_program_set_zip, so a host without zip
shows the step as skipped, by name, instead of passing without it (PS1B-397).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_program_set import SET_TOML, make_set  # noqa: E402


# The three shell helpers are the ones of test_package_setup_bios_hint.py. They
# are repeated here because that module does not import on Python 3.9 (the
# setup tools' floor), and this test must run there.
def find_bash():
    candidates = []
    found = shutil.which("bash")
    if found:
        candidates.append(found)
    if os.name == "nt":
        for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), r"C:\Program Files"):
            candidates += [os.path.join(base, "Git", "usr", "bin", "bash.exe"),
                           os.path.join(base, "Git", "bin", "bash.exe")]
    for c in candidates:
        # System32\bash.exe is the WSL launcher, not a shell for this repo's scripts.
        if os.path.isfile(c) and "system32" not in c.lower():
            return c
    return None


def shell_env(bash):
    """PATH that reaches the coreutils shipped beside `bash`."""
    env = dict(os.environ)
    here = os.path.dirname(bash)
    extra = [here, os.path.join(os.path.dirname(here), "bin"),
             os.path.join(os.path.dirname(here), "usr", "bin")]
    env["PATH"] = os.pathsep.join([p for p in extra if os.path.isdir(p)] + [env.get("PATH", "")])
    return env


def to_shell_path(path, bash):
    if os.name != "nt":
        return str(path)
    out = subprocess.run([bash, "-c", 'cygpath -u "$1"', "_", str(path)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace", env=shell_env(bash))
    return out.stdout.strip() or str(path)


def run(bash, root, *args):
    done = subprocess.run([bash, to_shell_path(PACKAGER, bash), "--root", to_shell_path(root, bash),
                           "--build-dir", "build-setup", "--artifact", "windows-x64", "--zip-prefix", "re2",
                           "--exe-name", "Resident_Evil_2", *args],
                          capture_output=True, text=True, encoding="utf-8", errors="replace", env=shell_env(bash))
    return done.returncode, (done.stdout + done.stderr).replace("\r", "")


def check_text() -> None:
    text = PACKAGER.read_text(encoding="utf-8")
    assert '--set) SET_FILE="${2:?}"; shift 2 ;;' in text
    # The set's root ships no mods catalog; each program's travels in its folder.
    assert text.index('if [[ -n "${SET_FILE}" ]]; then') < text.index('if [[ "${STAGE_MODS}" -eq 1 ]]; then')
    assert "  STAGE_MODS=0\nfi" in text
    # Setup's own work in a program folder is never packaged.
    assert "for _made in psxrecomp recomp-ui generated build-release .cache; do" in text
    assert 'rm -rf "${STAGE}/${_folder}/generated" "${STAGE}/${_folder}/disc"' in text
    # Every program's recipe passes the overlay_cache gate, not only a root game.toml.
    assert 'for _recipe in "${RECIPES[@]}"; do' in text
    assert 'RECIPES+=("${STAGE}/${_folder}/game.toml")' in text
    assert 'RECIPES=("${STAGE}/game.toml")' in text                      # a single package: as before
    assert 'BIOS_HINT="$(recipe_bios_hint "${STAGE}/game.toml")"' in text
    # A CMakeLists.txt with no if(EXISTS ...) guard must not end the packager silently.
    assert text.count("| sort -u || true)") >= 3
    # Developer-channel mods are pruned in each program's source catalog too.
    assert 'prune_dev_mods "${STAGE}/${_folder}/mods/preloaded/packages"' in text
    assert "Diagnostic mode and the optimised (PGO) rebuild are not available" in text
    # zip is handed a path relative to the stage, never one that begins at a mount.
    assert '    zip -r -q "../${ZIP_NAME}" .' in text
    assert 'zip -r -q "${DIST}' not in text


RECIPE = '''[game]
name = "Resident Evil 2 ({who})"
id = "{serial}"
exe = "disc/BOOT"
discs = ["disc/disc-1.cue"]
disc_serials = ["{serial}"]

[recompiler]
bios_config = "psxrecomp/bios/SCPH1001.toml"

[runtime]
openbios = false
overlay_cache = true
'''
PROGRAM_CMAKE = '''set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH "")
psxrecomp_add_game_runtime(psx-runtime
    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game_options.toml")
endif()
'''
PROGRAM_SETUP_C = '#include "codegen_setup.h"\n#include "psxrecomp_codegen_host.h"\n'
ROOT_NAMES = {"Resident_Evil_2", "set.toml", "CMakeLists.txt", "codegen_setup.c", "codegen_setup.h",
              "README-SETUP.txt", "VERSION", "psx_game_version.txt", "assets", "licenses", "programs",
              "psxrecomp", "recomp-ui"}
TOOLCHAIN_TEXTS = ("SDL3-LICENSE.txt", "llvm-mingw-LICENSE.TXT", "zlib-LICENSE.txt")


def put(path: Path, text: str = "stand-in\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def make_package_source(root: Path) -> Path:
    """A set's package source with stand-ins for everything a build makes: a
    host exe, the two emitters, and the smallest framework and launcher trees
    the packager's own checks accept. No compiler, no disc, no BIOS dump.
    Returns the stand-in recompiler build folder."""
    import program_set
    make_set(root)
    put(root / "VERSION", "0.4.0\n")
    for who, serial in (("leon", "SLUS-00748"), ("claire", "SLUS-00756")):
        folder = root / "programs" / who
        put(folder / "game.toml", RECIPE.format(who=who, serial=serial))
        put(folder / "CMakeLists.txt", PROGRAM_CMAKE)
        put(folder / "codegen_setup.c", PROGRAM_SETUP_C)
        put(folder / "codegen_setup.h", '#include "recomp_launcher.h"\n')
        put(folder / "seeds" / "funcs.txt")
        put(folder / "disc" / "left-over.txt")               # a working tree: never shipped
    put(root / "programs" / "leon" / "mods" / "preloaded" / "packages" / "dev.tool" / "1.0" / "manifest.toml",
        'id = "dev.tool"\nversion = "1.0"\nname = "Developer tool"\nchannel = "developer"\n')
    fw = root / "psxrecomp"
    for header in ("recompiler/lib/rabbitizer/include/generated/InstrId_enum.h",
                   "recompiler/lib/rabbitizer/include/generated/InstrDescriptor_Descriptors_array.h",
                   "recompiler/lib/rabbitizer/cplusplus/include/generated/UniqueId_enum_class.hpp"):
        put(fw / header)
    for name in ("psxrecomp_cli.py", "LICENSE", "host/psxrecomp_codegen_host.h", "runtime/runtime.cmake",
                 "bios/SCPH1001.toml", "bios/OpenBIOS.toml", "bios/openbios.bin", "bios/OpenBIOS.LICENSE"):
        put(fw / name)
    put(root / "recomp-ui" / "src" / "recomp_launcher.h")
    put(root / "recomp-ui" / "LICENSE")
    # The texts of the libraries the shipped binaries link: the release tool
    # writes them into the source, the packager carries the folder (PS1B-333).
    for name in TOOLCHAIN_TEXTS:
        put(root / "licenses" / "toolchain" / name, "the text of " + name + "\n")
    assert program_set.init_host(root) == list(program_set.HOST_FILES)
    build = root / "build-setup"
    put(build / "Resident_Evil_2", "stand-in host\n")
    put(build / "psx_game_version.txt", "0.4.0\n")
    put(build / "assets" / "fonts" / "font.ttf")
    put(build / "assets" / "img" / "mark.tga")
    emitters = root.parent / (root.name + "-recompiler-build")
    put(emitters / "psxrecomp-game")
    put(emitters / "psxrecomp-bios")
    put(emitters / "runtime" / "include" / "overlay_codegen_hash.h", "#define PSX_OVERLAY_CODEGEN_HASH 0x0u\n")
    return emitters


def package(bash, root, emitters, *args, env=None):
    """The whole packager, as Studio calls it for a set (a Linux artifact: the
    stand-in host has no imports to walk and nothing to sign)."""
    shell = shell_env(bash)
    shell.pop("CI", None)
    shell.update(env or {})
    done = subprocess.run(
        [bash, to_shell_path(PACKAGER, bash), "--root", to_shell_path(root, bash),
         "--build-dir", to_shell_path(root / "build-setup", bash), "--artifact", "linux-x64",
         "--zip-prefix", "workbench", "--exe-name", "Resident_Evil_2", "--display-name", "Resident Evil 2",
         "--set", to_shell_path(root / "set.toml", bash),
         "--recompiler-build", to_shell_path(emitters, bash), "--omit-openbios", *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace", env=shell,
        cwd=str(root))
    return done.returncode, (done.stdout + done.stderr).replace("\r", "")


def whole_packager(bash, tmp: Path) -> str:
    """Run the packager from its first line to the zip on a stand-in set, then
    on three sources it must refuse. Every step after the host lookup is
    reached here: the version stamp, asset and project staging, the framework
    and launcher trees, the private-payload and private-path gates, the SDK
    stage with its BIOS policy check, the overlay_cache gate, the BIOS wording,
    the readme, both configure gates, the include gate, the notice check and
    the zip."""
    root = tmp / "pkg"
    emitters = make_package_source(root)
    set_bytes = (root / "set.toml").read_bytes()
    code, out = package(bash, root, emitters)
    stage = root / "dist" / "stage-setup-linux-x64"
    zipped = "Wrote " in out
    # Whether the packager wrote a zip is not this machine's choice to make
    # quietly: it must match what the shell offers (PS1B-397).
    assert zipped == bool(zip_tool(bash)), (zip_tool(bash), code, out[-3000:])
    if not zipped:
        # A shell without zip stops at the last step; everything before it ran.
        # The zip itself is then checked by the --zip-part run (its own ctest
        # entry), which reports SKIP when no shell here has a zip.
        assert code == 1 and out.rstrip().endswith("error: zip not found"), (code, out[-3000:])
    else:
        assert code == 0, (code, out[-3000:])
    # The plain-start gate ran on the staged host. The stand-in is a text file,
    # which no system can start, so the gate must say that it checked nothing.
    assert "its plain start was NOT checked" in out, out[-3000:]
    # the BIOS policy step: each program's recipe, the profile from the stage root
    assert "staged BIOS policy: psxrecomp/bios/SCPH1001.toml" in out.replace("\\", "/"), out[-3000:]
    assert "stage_setup_sdk: ready" in out
    names = {p.name for p in stage.iterdir()}
    assert names == ROOT_NAMES, sorted(names ^ ROOT_NAMES)
    assert (stage / "set.toml").read_bytes() == set_bytes
    for who in ("leon", "claire"):
        folder = stage / "programs" / who
        assert sorted(p.name for p in folder.iterdir() if p.name != "mods") == [
            "CMakeLists.txt", "codegen_setup.c", "codegen_setup.h", "game.toml", "seeds"], sorted(folder.iterdir())
    # without the developer filter the program's source catalog is staged as it is
    assert (stage / "programs" / "leon" / "mods" / "preloaded" / "packages" / "dev.tool").is_dir()
    assert "developer-channel manifest" in out
    assert (stage / "psxrecomp" / "recompiler" / "build" / "psxrecomp-game").is_file()
    assert (stage / "psxrecomp" / "recompiler" / "build" / "psxrecomp-bios").is_file()
    assert (stage / "psxrecomp" / "bios" / "SCPH1001.toml").is_file()
    assert not (stage / "psxrecomp" / "bios" / "OpenBIOS.toml").exists()      # --omit-openbios
    assert (stage / "assets" / "fonts" / "font.ttf").is_file()
    # the package carries the toolchain libraries' licence texts at its root
    assert sorted(p.name for p in (stage / "licenses" / "toolchain").iterdir()) == sorted(TOOLCHAIN_TEXTS)
    assert (stage / "licenses" / "toolchain" / "SDL3-LICENSE.txt").read_text(encoding="utf-8") == \
        "the text of SDL3-LICENSE.txt\n"
    assert "staged licenses/ (3 file(s))" in out, out[-3000:]
    readme = (stage / "README-SETUP.txt").read_text(encoding="utf-8")
    assert "Resident Evil 2 0.4.0" in readme
    assert "This game's discs are separate programs" in readme
    assert "Provide every disc of your legally owned game and your own legally dumped SCPH1001 BIOS image" in readme
    assert "Diagnostic mode (if the game crashes" not in readme
    if zipped:
        check_zip(root, stage)

    # The developer filter reaches each program's source catalog.
    # A source without a licenses/ folder is still packaged, and the packager says what is missing.
    filtered = tmp / "filtered"
    emitters = make_package_source(filtered)
    shutil.rmtree(str(filtered / "licenses"))
    code, out = package(bash, filtered, emitters, env={"EXCLUDE_DEV_MODS": "1"})
    assert ("Wrote " in out) == zipped and "excluding developer-channel mods" in out, (code, out[-3000:])
    assert "this source has no licenses/ folder" in out, out[-3000:]
    assert not (filtered / "dist" / "stage-setup-linux-x64" / "licenses").exists()
    assert not (filtered / "dist" / "stage-setup-linux-x64" / "programs" / "leon" / "mods" / "preloaded"
                / "packages" / "dev.tool" / "1.0").exists()

    # A program's recipe without overlay_cache is refused, and named.
    cold = tmp / "cold"
    emitters = make_package_source(cold)
    recipe = cold / "programs" / "claire" / "game.toml"
    recipe.write_bytes(recipe.read_bytes().replace(b"overlay_cache = true\n", b""))
    code, out = package(bash, cold, emitters)
    assert code == 1 and "REFUSING TO PACKAGE" in out and "programs/claire/game.toml" in out, (code, out[-2000:])

    # A program's CMakeLists.txt that names a file the zip does not carry is refused.
    short = tmp / "short"
    emitters = make_package_source(short)
    cmake = short / "programs" / "leon" / "CMakeLists.txt"
    cmake.write_bytes(cmake.read_bytes() + b'add_library(x "${CMAKE_CURRENT_SOURCE_DIR}/src/missing.c")\n')
    code, out = package(bash, short, emitters)
    assert code == 1 and "programs/leon/src/missing.c" in out, (code, out[-2000:])

    # A program's recipe that needs a BIOS profile the stage lacks stops at the BIOS policy step.
    profile = tmp / "profile"
    emitters = make_package_source(profile)
    recipe = profile / "programs" / "claire" / "game.toml"
    recipe.write_bytes(recipe.read_bytes().replace(b"SCPH1001.toml", b"SCPH5552.toml"))
    code, out = package(bash, profile, emitters)
    assert code == 1 and "missing staged BIOS asset" in out and "SCPH5552.toml" in out, (code, out[-2000:])
    zip_gets_a_relative_path(bash, tmp)
    gate = plain_start_gate(bash, tmp, zipped)
    pick = exe_name_pick(tmp, zipped)
    return ("to the zip" if zipped else
            "to the zip step; the zip was NOT made here (no zip under this shell), see package_setup_program_set_zip"
            ) + "; " + gate + "; " + pick


def zip_tool(bash) -> str:
    """The zip the packager would call under this bash (`command -v zip`), or ''."""
    found = subprocess.run([bash, "-c", "command -v zip"], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=shell_env(bash))
    return found.stdout.strip() if found.returncode == 0 else ""


def check_zip(root: Path, stage: Path) -> str:
    """The zip the packager wrote: one file, its name, and exactly the staged files in it."""
    import zipfile
    archives = sorted((root / "dist").glob("*.zip"))
    assert [a.name for a in archives] == ["workbench-0.4.0-linux-x64.zip"], archives
    staged = sorted(p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file())
    with zipfile.ZipFile(archives[0]) as archive:
        assert archive.testzip() is None, "a member of the zip does not read back"
        members = sorted(name[2:] if name.startswith("./") else name
                         for name in archive.namelist() if not name.endswith("/"))
    assert members == staged, ("the zip does not hold exactly the staged files",
                               sorted(set(members) ^ set(staged))[:20])
    top = {name.split("/")[0] for name in members}
    assert top == ROOT_NAMES, sorted(top ^ ROOT_NAMES)
    return "%s, %d files" % (archives[0].name, len(members))


SKIP = 77      # ctest: SKIP_RETURN_CODE of package_setup_program_set_zip


def zip_part() -> int:
    """The packager's zip step, as its own ctest entry (PS1B-397).

    The main run accepts a shell without zip: it stops at "error: zip not
    found" and everything before that step is still checked. That made a host
    without zip pass with its zip step never run, and ctest said Passed. This
    run is only about the zip. It looks for a zip under every shell a build
    host runs the packager in, and where none has one it reports SKIP by name
    instead of PASS."""
    shells = windows_shells() if os.name == "nt" else [("bash", find_bash())]
    shells = [(name, bash) for name, bash in shells if bash]
    with_zip = [(name, bash, zip_tool(bash)) for name, bash in shells]
    with_zip = [entry for entry in with_zip if entry[2]]
    if not with_zip:
        looked = " and ".join(name for name, _ in shells) or "any shell (no bash was found)"
        print("SKIP: package setup program set test, zip part: no zip (Info-ZIP) is on PATH under " + looked
              + ". The packager's zip step was NOT RUN on this machine.")
        return SKIP
    name, bash, tool = with_zip[0]
    with tempfile.TemporaryDirectory(prefix="psxrecomp-set-packager-zip-", dir=working_folder(bash)) as tmp:
        root = Path(tmp) / "pkg"
        emitters = make_package_source(root)
        code, out = package(bash, root, emitters)
        assert code == 0 and "Wrote " in out, (name, code, out[-3000:])
        what = check_zip(root, root / "dist" / "stage-setup-linux-x64")
    print("package setup program set test, zip part: PASS under %s with %s (%s)" % (name, tool, what))
    return 0


def windows_shells():
    """Every bash a Windows build host runs the packager in: Git Bash and MSYS2."""
    found = []
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), r"C:\Program Files"):
        found.append(("Git Bash", os.path.join(base, "Git", "usr", "bin", "bash.exe")))
    for root in (os.environ.get("MSYS2_ROOT", ""), r"C:\msys64", r"D:\msys64"):
        if root:
            found.append(("MSYS2", os.path.join(root, "usr", "bin", "bash.exe")))
    shells, seen = [], set()
    for name, path in found:
        key = os.path.normcase(path)
        if os.path.isfile(path) and key not in seen and name not in [n for n, _ in shells]:
            seen.add(key)
            shells.append((name, path))
    return shells


def exe_name_pick(tmp: Path, zipped: bool) -> str:
    """The gate must hand its Python step a file that exists (PS1B-366).

    A Windows build folder holds Resident_Evil_2.exe and nothing named
    Resident_Evil_2. Under Git Bash and MSYS2, [[ -f X ]] is also true when
    only X.exe exists, so the packager's lookups settle on the name without the
    extension. The gate handed that path to python, which found no such file,
    and every Windows setup package stopped with "no such file ... no package
    was made". The earlier cases of this test could not see it: their stand-in
    host has no extension, and the gate's own cases are shell scripts that only
    a POSIX system starts.

    Here the build folder holds only the .exe name, under each Windows shell
    found. The stand-in is a text file, so it cannot be started: the right
    result is "NOT CHECKED" for the path that ends in .exe, and a package.
    Only a Windows shell has the rule, so off Windows the case is not run and
    says so.
    """
    if os.name != "nt":
        return ("exe name pick: NOT RUN here (only Git Bash and MSYS2 treat X as X.exe; "
                "the Windows gate runs this case)")
    shells = windows_shells()
    assert shells, "no Git Bash or MSYS2 bash was found: the exe name pick cannot be tested on this Windows host"
    ran = []
    for name, shell in shells:
        folder = working_folder(shell)
        with tempfile.TemporaryDirectory(prefix="psxrecomp-exe-pick-", dir=folder) as scratch:
            root = Path(scratch) / "exe-pick"
            emitters = make_package_source(root)
            build = root / "build-setup"
            (build / "Resident_Evil_2").rename(build / "Resident_Evil_2.exe")
            assert sorted(p.name for p in build.iterdir() if p.name.startswith("Resident_Evil_2")) == [
                "Resident_Evil_2.exe"]
            code, out = package(shell, root, emitters)
            assert "no such file" not in out, (
                name + ": the gate handed python a path that is not a file", out[-2500:])
            checked = [line for line in out.split("\n") if "setup host plain start:" in line]
            assert len(checked) == 1 and "NOT CHECKED" in checked[0], (name, checked, out[-2500:])
            handed = checked[0].rsplit("; ", 1)[-1].strip()
            if handed.startswith("/"):
                # a python of the shell's own family prints its own kind of path
                handed = subprocess.run([shell, "-c", 'cygpath -w "$1"', "_", handed], capture_output=True,
                                        text=True, encoding="utf-8", errors="replace",
                                        env=shell_env(shell)).stdout.strip() or handed
            assert handed.lower().endswith("resident_evil_2.exe"), (name, checked[0])
            assert os.path.isfile(handed), (name + ": the path handed to python is not a file", handed)
            assert "its plain start was NOT checked" in out, (name, out[-2500:])
            stage = root / "dist" / "stage-setup-linux-x64"
            assert (stage / "Resident_Evil_2.exe").is_file(), sorted(p.name for p in stage.iterdir())
            wrote = "Wrote " in out
            if not wrote:
                assert code == 1 and out.rstrip().endswith("error: zip not found"), (name, code, out[-2500:])
            else:
                assert code == 0, (name, code, out[-2500:])
            ran.append(name)
    return "exe name pick: the .exe file is handed to the gate under " + " and ".join(ran)


def plain_start_gate(bash, tmp: Path, zipped: bool) -> str:
    """A setup program that refuses its own config must stop the packager, and
    one that reaches its launcher must not (PS1B-365: the set package shipped
    with a setup program that exited before its window). The stand-in hosts are
    shell scripts, which only a POSIX system starts as programs."""
    if os.name == "nt":
        return "plain-start gate: stand-in programs not run (a Windows host must be a real executable)"
    refused = tmp / "refused"
    emitters = make_package_source(refused)
    host = refused / "build-setup" / "Resident_Evil_2"
    put(host, "#!/bin/sh\necho 'psxrecomp: failed to load --game set.toml: [error] key \"name\" not found' >&2\nexit 1\n")
    os.chmod(str(host), 0o755)
    code, out = package(bash, refused, emitters)
    assert code == 1 and "does not reach its launcher on a plain start; no package was made" in out, (code, out[-2000:])
    assert 'key "name" not found' in out, out[-2000:]
    assert not list((refused / "dist").glob("*.zip")), "a refused setup program must not be packaged"

    silent = tmp / "silent"
    emitters = make_package_source(silent)
    host = silent / "build-setup" / "Resident_Evil_2"
    put(host, "#!/bin/sh\nexit 0\n")
    os.chmod(str(host), 0o755)
    code, out = package(bash, silent, emitters)
    assert code == 1 and "exited with code 0 before host:before_run_window" in out, (code, out[-2000:])

    starts = tmp / "starts"
    emitters = make_package_source(starts)
    host = starts / "build-setup" / "Resident_Evil_2"
    put(host, "#!/bin/sh\necho '[boot-timing] +    0.0 ms  total     1.0 ms  host:before_run_window' >&2\nexec sleep 30\n")
    os.chmod(str(host), 0o755)
    code, out = package(bash, starts, emitters)
    assert "setup host plain start: PASS" in out and ("Wrote " in out) == zipped, (code, out[-2000:])
    assert sorted(p.name for p in (starts / "dist" / "stage-setup-linux-x64").iterdir()) == sorted(ROOT_NAMES)
    return "plain-start gate: refused, silent and starting stand-ins"


def zip_gets_a_relative_path(bash, tmp: Path) -> None:
    """The packager must hand zip a path relative to the stage. On Windows the
    zip on PATH can come from another shell family than the bash that runs the
    packager (Git Bash with MSYS2's zip); the two map /tmp to different
    folders, so an absolute path under the user's Temp folder failed with
    "zip error: Could not create output file (/tmp/...)" (PS1B-333). A stand-in
    zip, first on PATH, records what it is given."""
    root = tmp / "zip-arg"
    emitters = make_package_source(root)
    tools = tmp / "zip-arg-tools"
    seen = tmp / "zip-arg-seen.txt"
    put(tools / "zip", '#!/bin/sh\nprintf \'%s\\n\' "$PWD" "$@" > "$ZIP_ARGS_FILE"\nprintf zip > "$3"\n')
    os.chmod(str(tools / "zip"), 0o755)
    shell = shell_env(bash)
    code, out = package(bash, root, emitters, env={
        "PATH": to_shell_path(tools, bash) + ":" + subprocess.run(
            [bash, "-c", 'printf %s "$PATH"'], capture_output=True, text=True, encoding="utf-8",
            errors="replace", env=shell).stdout,
        "ZIP_ARGS_FILE": to_shell_path(seen, bash)})
    assert code == 0 and "Wrote " in out, (code, out[-2000:])
    cwd, flags, quiet, target, what = seen.read_text(encoding="utf-8").split("\n")[:5]
    assert (flags, quiet, what) == ("-r", "-q", "."), (flags, quiet, what)
    assert target == "../workbench-0.4.0-linux-x64.zip", target             # no folder of any mount in it
    assert cwd.replace("\\", "/").endswith("/dist/stage-setup-linux-x64"), cwd
    assert (root / "dist" / "workbench-0.4.0-linux-x64.zip").read_bytes() == b"zip"


def working_folder(bash):
    """Where the stand-in trees go: None for the default temporary folder.

    On Windows the default is under the user's Temp folder, which Git Bash
    mounts as /tmp and MSYS2 does not. A tool from the other family, called by
    the packager with such a path, looks in another folder. So the trees go to
    a folder whose path under this bash starts with a drive (/c/...): every
    shell family resolves that the same way.
    """
    if os.name != "nt":
        return None
    default = Path(tempfile.gettempdir())
    for base in (default, Path.home(), default.parent):
        shell_path = to_shell_path(base, bash)
        if not (len(shell_path) > 3 and shell_path[0] == "/" and shell_path[1].isalpha() and shell_path[2] == "/"
                and os.access(str(base), os.W_OK)):
            continue
        # The Python the packager finds must see the folder too. A Python
        # installed as a Windows app reads its own private copy of
        # AppData\Local, where a file this test wrote does not exist.
        with tempfile.TemporaryDirectory(prefix="psxrecomp-set-packager-probe-", dir=str(base)) as probe:
            put(Path(probe) / "probe.txt")
            seen = subprocess.run(
                [bash, "-c", 'for c in python3 python; do command -v "$c" >/dev/null 2>&1 && exec "$c" -c '
                             '"import sys; open(sys.argv[1]).read()" "$1"; done; exit 1',
                 "_", to_shell_path(Path(probe) / "probe.txt", bash)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", env=shell_env(bash))
        if seen.returncode == 0:
            return str(base)
    raise SystemExit("package setup program set test: cannot run. No writable folder was found that this bash ("
                     + bash + ") names by a drive path and that its python can read. The Temp folder is a mount "
                     "there (/tmp), which a zip or rsync from another shell family resolves elsewhere.")


def main() -> int:
    if "--zip-part" in sys.argv[1:]:
        return zip_part()
    check_text()
    bash = find_bash()
    if bash is None:
        print("package setup program set test: text checks PASS; SKIP the runs (no bash on this machine)")
        return 0
    syntax = subprocess.run([bash, "-n", to_shell_path(PACKAGER, bash)], capture_output=True, text=True,
                            env=shell_env(bash))
    assert syntax.returncode == 0, syntax.stderr
    with tempfile.TemporaryDirectory(prefix="psxrecomp-set-packager-test-", dir=working_folder(bash)) as tmp:
        root = Path(tmp) / "source"
        make_set(root)
        (root / "build-setup").mkdir()

        code, out = run(bash, root, "--set", "other.toml")
        assert code == 2 and "--set must be set.toml" in out, (code, out)

        # A set file setup would refuse is refused here, naming the key.
        bad = Path(tmp) / "bad"
        make_set(bad)
        (bad / "set.toml").write_bytes(
            SET_TOML.replace('shortcut = "Resident Evil 2 - Claire"\n', "").encode("utf-8"))
        (bad / "build-setup").mkdir()
        code, out = run(bash, bad, "--set", "set.toml")
        assert code == 1 and "[program.claire] shortcut" in out, (code, out)
        assert not (bad / "dist").exists(), "nothing may be staged for a refused set"

        # A source that was set up (the program folders hold links or game code) is refused.
        for made in ("psxrecomp", "generated"):
            used = Path(tmp) / ("used-" + made)
            make_set(used)
            (used / "build-setup").mkdir()
            (used / "programs" / "claire" / made).mkdir()
            code, out = run(bash, used, "--set", "set.toml")
            assert code == 1 and f"programs/claire/{made} exists" in out, (code, out)
            assert not (used / "dist").exists()

        # A valid set gets past every set check and stops where a single package
        # would: there is no built host in this test.
        code, out = run(bash, root, "--set", "set.toml")
        assert code == 1 and "setup host executable 'Resident_Evil_2' not found" in out, (code, out)
        # The set file may be given by path, as long as it is the root's set.toml.
        code, out = run(bash, root, "--set", to_shell_path(root / "set.toml", bash))
        assert code == 1 and "setup host executable 'Resident_Evil_2' not found" in out, (code, out)
        code, out = run(bash, root, "--set", str(root / "set.toml"))
        assert code == 1 and "setup host executable 'Resident_Evil_2' not found" in out, (code, out)
        code, out = run(bash, root, "--set", to_shell_path(Path(tmp) / "bad" / "set.toml", bash))
        assert code == 2 and "--set must be set.toml at the root" in out, (code, out)

        # Without --set nothing about a set is read: the same stop, same words.
        code, out = run(bash, root)
        assert code == 1 and "setup host executable 'Resident_Evil_2' not found" in out, (code, out)

        reached = whole_packager(bash, Path(tmp))
    print(f"package setup program set test: PASS (whole packager run {reached})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
