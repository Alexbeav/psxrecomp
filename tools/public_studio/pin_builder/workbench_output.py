"""Allowlisted private output and an external, run-bound development workspace."""
from copy import copy
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time
import tomllib
import uuid

try:
    from . import overlay_compile_check
    from . import pin_build as pb
    from . import product_residue
    from . import workbench_runs as runs
except ImportError:
    import overlay_compile_check
    import pin_build as pb
    import product_residue
    import workbench_runs as runs

RUNTIME_FILES = ("game.toml", "game_options.toml", "disc.cfg", "bios.cfg", "settings.toml",
                 "overlay_codegen_hash.h", "psx_game_version.txt")
RUNTIME_DIRS = ("assets", "mods", "overlay_toolchain")
# Folders of a staged runtime directory that no player needs. The pinned tcc
# archive is unpacked whole and brings its own example programs: five .c files
# nothing reads. Leaving them out is what lets the rule below be simple.
LEFT_OUT_OF_PAYLOAD = ("overlay_toolchain/tcc/examples",)
# A product holds no source files, except the ones named here, each with its
# reason. "Source" is .c, .cpp, .h and .inc. The rule is the guard the first
# program-set build lacked: its delivered folder held 63 generated shard
# sources (game code) and nothing refused them (PS1B-333). A pattern ending in
# "/**" covers a folder and everything under it.
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".inc")
SOURCE_ALLOWLIST = (
    ("overlay_codegen_hash.h",
     "the runtime checks its overlay code generation hash against this header before it loads a cached overlay"),
    ("overlay_toolchain/include/**",
     "the runtime headers the bundled compiler includes when it compiles an overlay or BIOS shard on the player's machine"),
    ("overlay_toolchain/tcc/include/**",
     "tcc's own C headers; every tcc compile needs them"),
    ("overlay_toolchain/tcc/libtcc/libtcc.h",
     "the interface header of libtcc.dll, part of the pinned tcc archive; nothing reads it at run time, and it stays "
     "because only the examples were decided to go"),
    ("overlay_toolchain/clang/**",
     "a bundled clang carries its own builtin headers (products that bundle the compiler instead of using the shared store)"),
)


def lean_recipe(work, receipt):
    """Normalize only the two canonical wizard opt-ins in the copied recipe."""
    path = work / "CMakeLists.txt"
    before = pb.identity(path)
    text = path.read_text(encoding="utf-8-sig")
    lines = []
    changes = []
    source_lines = iter(text.splitlines(keepends=True))
    for line in source_lines:
        active = line.strip()
        if not active or active.startswith("#"):
            lines.append(line)
            continue
        if "PSX_SETUP_WIZARD" in active:
            if re.fullmatch(r'set\(PSX_SETUP_WIZARD (?:ON|OFF) CACHE BOOL', active):
                continuation = next(source_lines, "")
                line += continuation
                active += " " + continuation.strip()
            match = re.fullmatch(r'set\(PSX_SETUP_WIZARD (ON|OFF) CACHE BOOL "[^"\r\n]*" FORCE\)', active)
            if not match:
                raise pb.BuildStop("The copied kit has an unfamiliar PSX_SETUP_WIZARD declaration; cannot safely make a lean product.")
            if match[1] == "ON":
                line = line.replace("PSX_SETUP_WIZARD ON", "PSX_SETUP_WIZARD OFF", 1)
                changes.append("PSX_SETUP_WIZARD ON -> OFF")
        if "ENABLE_SETUP_WIZARD" in active:
            if active != "ENABLE_SETUP_WIZARD":
                raise pb.BuildStop("The copied kit has an unfamiliar ENABLE_SETUP_WIZARD argument; cannot safely disable generation controls.")
            changes.append("Removed standalone ENABLE_SETUP_WIZARD argument")
            continue
        lines.append(line)
    path.write_text("".join(lines), encoding="utf-8")
    receipt["lean_recipe"] = {"before": before, "after": pb.identity(path), "changes": changes,
                              "original_kit_unchanged": True, "runtime_sources_unchanged": True}


def recipe_netplay(recipe):
    """True when the kit recipe turns netplay on before it includes the runtime.

    The kit is the switch (PS1B-278). The runtime decides at its include whether
    the netplay sources compile for real or as stubs, so only an active
    `set(PSX_NETPLAY ON ...)` above that include counts; the last one wins.
    ENABLE_NETPLAY_IF_PRESENT alone leaves netplay stubbed.
    """
    if not recipe.is_file():
        return False
    wanted = False
    for line in recipe.read_text(encoding="utf-8-sig").splitlines():
        active = line.strip()
        if not active or active.startswith("#"):
            continue
        switch = re.match(r"set\s*\(\s*PSX_NETPLAY\s+(ON|OFF)\b", active)
        if switch:
            wanted = switch[1] == "ON"
        if active.startswith("include") and "runtime/runtime.cmake" in active:
            break
    return wanted


def compile_identity(ninja):
    """The framework pin and lobby version stamped on the compile lines."""
    def one(name):
        values = sorted(set(re.findall(r'-D' + name + r'=\\?"([^"\\\s]*)\\?"', ninja)))
        if len(values) > 1:
            raise pb.BuildStop(f"The build stamps more than one {name}: {', '.join(values)}.")
        return values[0] if values else None
    return one("PSX_FRAMEWORK_PIN"), one("PSX_GAME_VERSION")


NETPLAY_SWITCHES = ("PSX_HAS_RECOMP_NET", "PSX_HAS_LOBBY_CLIENT")


def netplay_switches(definitions):
    """The netplay switches that are on in a build's compile-line definitions."""
    return [name for name in NETPLAY_SWITCHES if any(f"-D{name}=1" in d for d in definitions)]


def compile_facts(ninja):
    """What a build's own compile lines say, from build.ninja text (or the lines of it that carry these
    names): {"netplay": "on" | "off" | "partial (<switch>)", "framework_pin", "lobby_version"}.

    The Windows product's record takes netplay from the kit recipe and checks it against the compile
    lines (verify_lean_compile). A Linux or macOS product is another build of the same recipe on
    another host, so its record states what that build's compile lines say, not what Windows did.
    """
    definitions = sorted(set(re.findall(r"(?m)^\s*DEFINES = (.*)$", ninja)))
    linked = netplay_switches(definitions)
    netplay = "on" if len(linked) == len(NETPLAY_SWITCHES) else "off" if not linked else f"partial ({', '.join(linked)})"
    framework_pin, lobby_version = compile_identity(ninja)
    return {"netplay": netplay, "framework_pin": framework_pin, "lobby_version": lobby_version}


def verify_lean_compile(product, receipt):
    cache = (product / "CMakeCache.txt").read_text(encoding="utf-8")
    ninja = (product / "build.ninja").read_text(encoding="utf-8")
    definitions = sorted(set(re.findall(r"(?m)^\s*DEFINES = (.*)$", ninja)))
    if not re.search(r"(?m)^PSX_SETUP_WIZARD:BOOL=OFF$", cache) or any("PSX_HAS_SETUP_WIZARD" in d for d in definitions):
        raise pb.BuildStop("Lean build still enables setup/generate/Optimize callbacks; refusing to stage it.")
    if receipt.get("ui_module_build") and not (re.search(r"(?m)^PSX_LAUNCHER_MODULE:BOOL=ON$", cache)
                                               and re.search(r"(?m)^PSX_STATIC_RUNTIME:BOOL=OFF$", cache)):
        # PS1B-482: the module options must be what CMake kept, or the product is the bundled launcher.
        raise pb.BuildStop("A launcher-module UI was selected, but the build did not configure PSX_LAUNCHER_MODULE ON "
                           "with PSX_STATIC_RUNTIME OFF (CMakeCache.txt). Refusing to stage it.")
    # Netplay: what the kit asks for must be what the build links. A title flag
    # that silently stays stubbed ships a release with no NETPLAY button.
    wanted = recipe_netplay(product.parent / "CMakeLists.txt")
    linked = netplay_switches(definitions)
    if wanted and len(linked) != 2:
        raise pb.BuildStop("The kit turns netplay on, but the build left it stubbed: PSX_HAS_RECOMP_NET and "
                           "PSX_HAS_LOBBY_CLIENT are not both on the compile lines. Check that the pin carries "
                           "lib/recomp-net and lib/retcomm-rbengine, and that the kit sets PSX_NETPLAY before "
                           "it includes runtime.cmake.")
    if linked and not wanted:
        raise pb.BuildStop("The build links netplay (" + ", ".join(linked) + "), but the kit does not turn it on. "
                           "Find what set PSX_NETPLAY before staging this product.")
    # Identity: a pin that carries runtime/FRAMEWORK_PIN stamps its own commit
    # (PS1B-287). The lobby matches rooms on the version built from it (PS1B-295).
    framework_pin, lobby_version = compile_identity(ninja)
    expected = receipt.get("pin", {}).get("commit")
    self_identifying = (product.parent / "psxrecomp/runtime/FRAMEWORK_PIN").is_file()
    if self_identifying and expected and framework_pin != expected:
        raise pb.BuildStop(f"The build stamps framework pin {framework_pin or '(none)'}, not the selected pin "
                           f"{expected}. Replays and netplay rooms would not match other builds of this pin.")
    if wanted and self_identifying and expected and not (lobby_version or "").endswith("-" + expected[:8]):
        raise pb.BuildStop(f"The netplay lobby version is {lobby_version or '(none)'}; it must end with "
                           f"-{expected[:8]}, so only builds of this pin share a room.")
    receipt["lean_compile"] = {"PSX_SETUP_WIZARD": "OFF", "definitions": definitions,
                               **({"PSX_LAUNCHER_MODULE": "ON", "PSX_STATIC_RUNTIME": "OFF"}
                                  if receipt.get("ui_module_build") else {}),
                               "ninja_sha256": pb.sha(product / "build.ninja"),
                               "netplay": "on" if wanted else "off",
                               "framework_pin": framework_pin, "lobby_version": lobby_version}


def pgo_result(log, receipt):
    results = []
    for line in Path(log).read_text(encoding="utf-8").splitlines():
        try:
            value = json.loads(line)
            if isinstance(value, dict) and value.get("event") == "result" and "pgo" in value:
                results.append(value)
        except ValueError:
            continue
    receipt["pgo_result"] = results[-1] if results else None
    if not results or results[-1].get("pgo") is not True or results[-1].get("pgo_skipped"):
        raise pb.BuildStop("Requested PGO did not complete: " + str(results[-1].get("pgo_skipped") if results else "no final PGO result"))


def copy_allowed(source, target):
    """Do not follow links outside the build-staged runtime payload."""
    if source.is_symlink() or source.is_junction():
        raise pb.BuildStop(f"Runtime payload cannot contain a link: {source}")
    if source.is_dir():
        for path in source.rglob("*"):
            if path.is_symlink() or path.is_junction():
                raise pb.BuildStop(f"Runtime payload cannot contain a link: {path}")
        shutil.copytree(source, target)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def leave_out_of_payload(target, receipt):
    """Remove the folders no player needs from a staged payload and record them."""
    removed = []
    for relative in LEFT_OUT_OF_PAYLOAD:
        folder = Path(target) / relative
        if folder.is_dir():
            files = [p for p in folder.rglob("*") if p.is_file()]
            removed.append({"path": relative, "files": len(files), "bytes": sum(p.stat().st_size for p in files)})
            shutil.rmtree(folder)
    if removed:
        receipt["payload_left_out"] = removed


def stray_sources(product):
    """Source files in `product` that the allowlist does not cover, as sorted relative paths."""
    product = Path(product)
    exact = {pattern for pattern, _reason in SOURCE_ALLOWLIST if not pattern.endswith("/**")}
    folders = tuple(pattern[:-2] for pattern, _reason in SOURCE_ALLOWLIST if pattern.endswith("/**"))
    found = []
    for path in product.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(product).as_posix()
        if relative in exact or relative.startswith(folders):
            continue
        found.append(relative)
    return sorted(found)


def assert_no_stray_sources(product):
    """Refuse a product that holds a source file outside the allowlist.

    Generated shard sources are game code. A start writes them into the folder
    it runs from; the staging and collect steps are what keep them out of a
    product, and this is the check that they did.
    """
    found = stray_sources(product)
    if found:
        shown = ", ".join(found[:8]) + (f" and {len(found) - 8} more" if len(found) > 8 else "")
        raise pb.BuildStop(
            f"The product holds {len(found)} source file(s) that may not ship: {shown}. A product carries no "
            ".c, .cpp, .h or .inc file except the listed runtime headers. Generated shard sources are game code; "
            "rebuild from a clean run, or add the file to the allowlist with its reason if a player needs it.")


def verify_staged_ui(target, receipt):
    """Hash the staged launcher module, shared SDL and asset receipt against the selected Build.

    The expected values come from the Build's ui_host_contract, never from the
    UI record; the first mismatching identity stops the build (PS1B-327).
    """
    selection = receipt.get("ui_selection")
    if not selection:
        raise pb.BuildStop("The build receipt names no UI selection, so the staged UI cannot be checked. "
                           "Start the build again from Studio or the pin_build command, which select it.")
    try:
        return pb.ui_selection.verify_staged(receipt["pin"], selection, target)
    except pb.ui_selection.UiSelectionError as error:
        raise pb.BuildStop(str(error)) from error


PE_IMPORT = re.compile(r"DLL Name:\s*(\S+)")


def pe_imports(objdump, binary):
    """The DLL names a PE file imports, read with the toolchain's llvm-objdump."""
    proc = subprocess.run([str(objdump), "-p", str(binary)], capture_output=True, text=True, encoding="utf-8",
                          errors="replace", creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if proc.returncode:
        raise pb.BuildStop(f"llvm-objdump could not read the imports of {binary}: {proc.stderr.strip()[:300]}")
    return PE_IMPORT.findall(proc.stdout)


def stage_module_runtime(target, exe_name, receipt, imports=None, system=None):
    """Copy the non-system DLLs that the executable, the module and SDL3 import, from the toolchain.

    A module build links the C++ runtime dynamically (PSX_STATIC_RUNTIME OFF),
    so the executable imports libc++.dll and libunwind.dll. Each staged binary's
    imports are followed; Windows API sets and System32 DLLs are left to
    Windows; a DLL that neither the product nor the toolchain has stops the build.
    """
    imports = pe_imports if imports is None else imports
    toolchain = Path(receipt["toolchain"]["root"])
    objdump = toolchain / "bin" / "llvm-objdump.exe"
    system = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" if system is None else Path(system)
    contract = receipt["pin"]["ui_host_contract"]
    queue = [target / exe_name, target / contract["module"]["file"], target / contract["shared_sdl"]["file"]]
    seen, added = set(), []
    while queue:
        binary = queue.pop(0)
        if binary.name.lower() in seen or not binary.is_file():
            continue  # a missing module or SDL is named by verify_staged_ui next
        seen.add(binary.name.lower())
        for name in imports(objdump, binary):
            if re.match(r"(?i)(api|ext)-ms-win-", name) or (system / name).is_file():
                continue
            staged = target / name
            if not staged.is_file():
                source = next((folder / name for folder in (toolchain / "x86_64-w64-mingw32" / "bin", toolchain / "bin")
                               if (folder / name).is_file()), None)
                if source is None:
                    raise pb.BuildStop(f"{binary.name} imports {name}, which neither the product nor the toolchain "
                                       f"{toolchain} has. The launcher-module product cannot start without it.")
                copy_allowed(source, staged)
                added.append({"file": name, "source": str(source), "sha256": pb.sha(staged), "for": binary.name})
            queue.append(staged)
    return added


def stage_payload(work, target, receipt):
    product = work / "build-release"
    exe = Path(receipt["executable"]["path"])
    required = (exe.name, "game.toml", "disc.cfg", "bios.cfg", "settings.toml", "overlay_codegen_hash.h", "overlay_toolchain", "assets", "mods")
    for name in required:
        if not (product / name).exists():
            raise pb.BuildStop(f"Runtime payload is missing {name}.")
    for name in (exe.name, *RUNTIME_FILES, *RUNTIME_DIRS):
        if (product / name).exists():
            copy_allowed(product / name, target / name)
    for path in sorted(product.glob("*.dll")):
        copy_allowed(path, target / path.name)
    leave_out_of_payload(target, receipt)
    if receipt.get("ui_module_build"):
        # PS1B-482: a launcher-module product imports the C++ runtime DLLs and carries its asset receipt.
        receipt["module_runtime_dlls"] = stage_module_runtime(target, exe.name, receipt)
        from asset_preflight import canonical_icon
        icon = canonical_icon(work / "psxrecomp", target)
        if icon:
            receipt["ui_canonical_icon"] = icon
        contract = receipt["pin"]["ui_host_contract"]
        receipt["ui_asset_receipt"] = pb.ui_selection.write_asset_receipt(
            target / "assets", target / contract["assets"]["receipt_file"])
    # Module, shared SDL and assets are in place: check their bytes before anything else is staged.
    receipt["ui_staged_identity"] = verify_staged_ui(target, receipt)
    if receipt["input_mode"] == "portable":
        copy_allowed(product / "inputs", target / "inputs")
    (target / "saves").mkdir()
    # Retain licenses from exactly the kit/framework/UI inputs used by this run.
    # A census-scaffold build has no kit, so it contributes no kit licenses.
    roots = [("framework", work / "psxrecomp"), ("ui", work / "recomp-ui")]
    if receipt.get("kit"):
        roots.insert(0, ("kit", Path(receipt["kit"])))
    for label, root in roots:
        for path in root.rglob("*"):
            if path.is_file() and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "THIRD_PARTY")):
                if any(part in (".git", "build", "build-release") for part in path.relative_to(root).parts):
                    continue
                copy_allowed(path, target / "licenses" / label / path.relative_to(root))
    stage_bundled_notices(target, receipt)
    stage_linked_notices(work, product, exe.name, target / "licenses", receipt)
    prepare_overlay_compiler(target, receipt)
    receipt["payload_members"] = sorted(p.name for p in target.iterdir())
    receipt["payload_allowlist"] = {"files": list(RUNTIME_FILES), "directories": list(RUNTIME_DIRS),
                                    "dependencies": "build-staged root DLLs", "media": receipt["input_mode"]}


# Third-party static archives a player binary may link, and the token a notice
# file under licenses/ must carry for each (PS1B-300). libchdr's notice covers the
# lzma, miniz and zstd it bundles. Archives built from the framework, kit or UI
# sources are covered by those trees' own licences.
LINKED_NOTICES = (
    (re.compile(r"(^|/)libchdr[^/]*\.a$|/psx_libchdr-build/deps/"), "libchdr"),
    (re.compile(r"(^|/)libSDL3[^/]*\.a$", re.I), "sdl3"),
    (re.compile(r"(^|/)libz\.a$"), "zlib"),
    (re.compile(r"juice[^/]*\.a$", re.I), "libjuice"),
)


def linked_archives(build_ninja, exe_name):
    """Static archives on the player executable's link line in build.ninja."""
    lines = Path(build_ninja).read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(lines):
        if line.startswith("build ") and f"{exe_name}:" in line.replace("$:", ":") and "LINKER" in line:
            for follow in lines[i + 1:]:
                if not follow.startswith("  "):
                    break
                if follow.strip().startswith("LINK_LIBRARIES ="):
                    return sorted({token.replace("\\", "/") for token in follow.split("=", 1)[1].split()
                                   if token.lower().endswith((".a", ".lib"))})
    raise pb.BuildStop(f"Cannot find the link line for {exe_name} in {build_ninja}; the licence check cannot run.")


# The licence texts of the libraries the pinned toolchain pack supplies and
# every player and setup program links: SDL3, zlib and the compiler's runtime.
TOOLCHAIN_NOTICES = ("SDL3-LICENSE.txt", "zlib-LICENSE.txt", "llvm-mingw-LICENSE.TXT")


def stage_toolchain_notices(toolchain, folder):
    """Write the toolchain pack's licence texts into `folder`; return the names written.

    One place for the bytes: a private product's licenses/toolchain and a setup
    package's source (workbench_package.write_package_notices) call this, so
    the folder a setup installs can be compared with the private product byte
    for byte (PS1B-333). A text the pack does not have is not written and not
    in the returned list; the caller decides what that means.
    """
    toolchain, folder = Path(toolchain), Path(folder)
    written = []
    sdl = toolchain / "deps" / "share" / "licenses" / "SDL3" / "LICENSE.txt"
    if sdl.is_file():
        copy_allowed(sdl, folder / "SDL3-LICENSE.txt")
        written.append("SDL3-LICENSE.txt")
    zlib_h = toolchain / "deps" / "include" / "zlib.h"
    if zlib_h.is_file():
        # zlib ships its licence as the opening comment of zlib.h.
        match = re.search(r"/\*\s*zlib\.h -- .*?\*/", zlib_h.read_text(encoding="utf-8", errors="replace"), re.S)
        if match:
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "zlib-LICENSE.txt").write_text(
                "zlib licence, from the opening comment of zlib.h in the pinned toolchain pack.\n\n"
                + match.group(0) + "\n", encoding="utf-8")
            written.append("zlib-LICENSE.txt")
    mingw = toolchain / "LICENSE.TXT"
    if mingw.is_file():
        copy_allowed(mingw, folder / "llvm-mingw-LICENSE.TXT")
        written.append("llvm-mingw-LICENSE.TXT")
    return written


def stage_linked_notices(work, product, exe_name, licenses, receipt):
    """Ship the notices of every third-party library linked into the player (PS1B-300).

    The name rule above skips runtime/licenses/<name>-NOTICES.txt, which the
    release packagers copy wholesale, and SDL3 and zlib come from the toolchain
    pack, not a source tree. Copy them, then stop the build when a linked
    third-party archive has no notice, as tools/package_release_macos.sh does.
    Linux and macOS products copy this licenses/ folder from the Windows payload.
    """
    notices = work / "psxrecomp" / "runtime" / "licenses"
    if notices.is_dir():
        for path in sorted(notices.iterdir()):
            if path.is_file():
                copy_allowed(path, licenses / "framework" / "runtime" / "licenses" / path.name)
    toolchain = Path(receipt["toolchain"]["root"]) if receipt.get("toolchain") else None
    if toolchain is not None:
        stage_toolchain_notices(toolchain, licenses / "toolchain")
    shipped =[p.relative_to(licenses).as_posix().lower() for p in licenses.rglob("*") if p.is_file()]
    linked = linked_archives(product / "build.ninja", exe_name)
    checked, missing = [], []
    for archive in linked:
        internal = "/_deps/" not in "/" + archive and (toolchain is None or
                                                 str(toolchain).replace("\\", "/").lower() not in archive.lower())
        token = next((t for pattern, t in LINKED_NOTICES if pattern.search(archive)), None)
        if token is None:
            if internal:
                continue  # built from the framework, kit or UI sources
            missing.append(f"{archive} (no known notice)")
            continue
        found = [s for s in shipped if token in s]
        checked.append({"archive": archive, "notice": found[0] if found else None})
        if not found:
            missing.append(f"{archive} (needs a {token} notice)")
    receipt["licence_check"] = {"linked": checked, "shipped": sorted(shipped)}
    if missing:
        raise pb.BuildStop("Linked third-party libraries have no licence notice in the product: "
                           + "; ".join(missing) + ". Add the notice before shipping.")


# Programs a product carries as files of their own, and the licence file each
# must have in the product (PS1B-345). The check above covers libraries linked
# into the player; these are separate programs the player's machine runs.
#   marker   a file that is there when the program is bundled
#   licence  the file the product must then hold
TCC_VERSION = "0.9.27"
TCC_ARCHIVE = "tcc-0.9.27-win64-bin.zip"  # the pin's tools/release_stage.py names it and its hash
TCC_ARCHIVE_SHA256 = "34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2"
TCC_RELEASES = "https://download.savannah.gnu.org/releases/tinycc/"
# The GNU Lesser General Public License, version 2.1. The pinned tcc archive
# holds no COPYING file, so the text is carried here. These are the bytes that
# xz, gettext, libiconv and libtasn1 ship as their LGPL-2.1 text (Git for
# Windows and MSYS2 on Eagle, 2026-10-01); the hash is checked before staging.
LGPL_21 = Path(__file__).with_name("licenses") / "LGPL-2.1.txt"
LGPL_21_SHA256 = "20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95"
TCC_NOTICE_SHA256 = "7eb9c42aeebb28b5fec05fc065a4c22808aedfccd344ba3e75674d06f21e1835"  # of tcc_notice(), UTF-8
BUNDLED_PROGRAMS = (
    {"name": f"TinyCC {TCC_VERSION}", "licence_name": "LGPL-2.1", "marker": "overlay_toolchain/tcc/tcc.exe",
     "licence": "licenses/toolchain/tcc-COPYING.txt", "sha256": LGPL_21_SHA256},
    {"name": "CPython (embedded)", "licence_name": "PSF licence", "marker": "overlay_toolchain/python/python.exe",
     "licence": "overlay_toolchain/python/LICENSE.txt", "sha256": None},
)


def tcc_notice():
    """What the product says about the tcc it holds: which release, where its source is.

    One wording that is true in both cases: a private product has the archive
    unpacked on the build host; a public setup package does not contain tcc,
    and its setup downloads and unpacks the archive on the player's machine.
    The framework writes the same bytes there (tools/release_stage.py on
    psxrecomp branch h/setup-package-licences); TCC_NOTICE_SHA256 pins them in
    both trees.
    """
    return (f"TinyCC (tcc) {TCC_VERSION}\n\n"
            "This product holds the TinyCC compiler in overlay_toolchain/tcc. The game starts it as a\n"
            "separate program to compile code on this machine; the game does not link it.\n\n"
            f"Binaries: unmodified, unpacked from {TCC_ARCHIVE}\n"
            f"  (SHA-256 {TCC_ARCHIVE_SHA256}),\n"
            f"  {TCC_RELEASES}\n"
            "  The archive was unpacked when this product was built, or by its setup on this machine,\n"
            "  which downloads it from that address. The archive's examples folder is not kept.\n"
            f"Source:   tcc-{TCC_VERSION}.tar.bz2, at the same address.\n"
            "Licence:  GNU Lesser General Public License, version 2.1. The text is tcc-COPYING.txt in this folder.\n")


def stage_bundled_notices(target, receipt):
    """Put the licence of every bundled program into the staged payload (PS1B-345).

    The pinned tcc archive says "See COPYING file" and has none, so a product
    carried tcc with a statement of its licence and no licence text.
    """
    target = Path(target)
    staged = []
    if (target / "overlay_toolchain/tcc/tcc.exe").is_file():
        if not LGPL_21.is_file() or pb.sha(LGPL_21) != LGPL_21_SHA256:
            raise pb.BuildStop(f"The LGPL-2.1 text Workbench ships with the bundled tcc is missing or changed: {LGPL_21}. "
                               "Restore the file from the Studio checkout.")
        folder = target / "licenses" / "toolchain"
        folder.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(LGPL_21, folder / "tcc-COPYING.txt")
        (folder / "tcc-NOTICE.txt").write_text(tcc_notice(), encoding="utf-8", newline="\n")
        staged += ["licenses/toolchain/tcc-COPYING.txt", "licenses/toolchain/tcc-NOTICE.txt"]
    if staged:
        receipt["bundled_notices_staged"] = staged


def bundled_notice_gaps(product):
    """Bundled programs in `product` whose licence file is absent or wrong, as text lines."""
    product = Path(product)
    gaps = []
    for program in BUNDLED_PROGRAMS:
        if not (product / program["marker"]).is_file():
            continue
        licence = product / program["licence"]
        if not licence.is_file() or licence.stat().st_size == 0:
            gaps.append(f"{program['name']} ({program['marker']}) needs {program['licence']} ({program['licence_name']})")
        elif program["sha256"] and pb.sha(licence) != program["sha256"]:
            gaps.append(f"{program['name']}: {program['licence']} is not the {program['licence_name']} text")
    return gaps


def record_bundled_notice_gaps(product, receipt):
    """Record, as a warning, every bundled program whose licence file the product lacks.

    A warning, not a stop (Alex, 2026-10-01): the files are written by the same
    step that stages the program, so a miss is unlikely, and for the public
    release the tcc binaries are not ours to distribute at all (the player's
    machine downloads them at setup). The gaps go into the receipt and the
    result row, so a wave summary can list them.
    """
    gaps = bundled_notice_gaps(product)
    if gaps:
        receipt["bundled_notice_gaps"] = gaps
        receipt.setdefault("warnings", []).append(
            "Bundled programs have no licence file in the product: " + "; ".join(gaps) + ".")
    return gaps


CLANG_BIN = ("clang.exe", "gcc.exe", "clang-22.exe", "ld.lld.exe", "libLLVM-22.dll",
             "libclang-cpp.dll", "libc++.dll", "libunwind.dll",
             "x86_64-w64-windows-gnu.cfg", "mingw32-common.cfg")
CLANG_LIB = ("dllcrt2.o", "crtbegin.o", "crtend.o", "libmingw32.a", "libunwind.a",
             "libmoldname.a", "libmingwex.a", "libmsvcrt.a", "libadvapi32.a",
             "libshell32.a", "libuser32.a", "libkernel32.a", "libm.a")


@contextmanager
def shared_compiler_guard(store, exclusive=False):
    if os.name != "nt":
        raise pb.BuildStop("Shared overlay Clang currently requires Windows.")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                  ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(store) + ".lock", 0xC0000000 if exclusive else 0x80000000,
                                0 if exclusive else 1, None, 4, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        error = ctypes.get_last_error()
        raise pb.BuildStop(f"Shared compiler {store} is in use or cannot be locked (Windows error {error}); close its games and retry.")
    try:
        yield
    finally:
        kernel.CloseHandle(handle)


def install_shared_compiler(source, entries, root, repair=False):
    # AppData may be an existing junction; perform creation at its real target.
    root = root.resolve()
    entries = sorted(entries, key=lambda item: item["path"])
    manifest = "".join(f"{item['sha256']}  {item['path']}\n" for item in entries).encode("utf-8")
    content_hash = hashlib.sha256(manifest).hexdigest()
    store = root / ("overlay-clang-" + content_hash)
    def verify(directory):
        try:
            if (directory / "SHA256SUMS.txt").read_bytes() != manifest:
                raise ValueError("manifest differs")
            for item in entries:
                path = directory / item["path"]
                if path.is_symlink() or path.stat().st_size != item["bytes"] or pb.sha(path) != item["sha256"]:
                    raise ValueError("dependency differs: " + item["path"])
        except (OSError, ValueError) as error:
            raise pb.BuildStop(f"Shared compiler store {directory} is missing or corrupt ({error}); re-run the Workbench with --repair-shared-toolchain, or rebuild as portable.") from error
    copied_files = copied_bytes = 0
    root.mkdir(parents=True, exist_ok=True)
    quarantine = None
    with shared_compiler_guard(store, exclusive=repair):
        reused = store.exists()
        if reused:
            try:
                verify(store)
            except pb.BuildStop:
                if not repair:
                    raise
                quarantine = store.with_name(store.name + ".quarantine-" + uuid.uuid4().hex)
                store.rename(quarantine)
                reused = False
        if not reused:
            staging = Path(tempfile.mkdtemp(prefix=".install-overlay-clang-", dir=root))
            for item in entries:
                copy_allowed(source / item["path"], staging / item["path"])
                copied_files += 1; copied_bytes += item["bytes"]
            (staging / "SHA256SUMS.txt").write_bytes(manifest)
            verify(staging)
            try:
                staging.rename(store)
            except OSError:
                if not store.exists():
                    raise
                # A concurrent installer won. Verify it, retain our staging bytes.
                verify(store)
                reused = True
    return {"root": str(store), "content_sha256": content_hash, "reused": reused,
            "quarantine": str(quarantine) if quarantine else None,
            "copied_files": copied_files, "copied_bytes": copied_bytes,
            "payload_bytes": sum(item["bytes"] for item in entries)}


def prepare_overlay_compiler(target, receipt):
    source = Path(receipt["toolchain"]["root"])
    metadata = receipt["toolchain"]["metadata"]
    if (metadata.get("id"), metadata.get("version"), metadata.get("os")) != ("cmake-clang-v1", "1.0.14", "windows-x64"):
        raise pb.BuildStop("The portable Clang subset is defined for pinned Windows cmake-clang-v1 1.0.14 only.")
    relative = [Path("bin") / name for name in CLANG_BIN]
    relative += [Path("x86_64-w64-mingw32/lib") / name for name in CLANG_LIB]
    relative += [Path("lib/clang/22/lib/windows/libclang_rt.builtins-x86_64.a"), Path("LICENSE.TXT"), Path("retcomm-toolchain.json")]
    for directory in ("lib/clang/22/include", "include"):
        relative += [p.relative_to(source) for p in (source / directory).rglob("*")
                     if p.is_file() and "c++" not in p.relative_to(source / directory).parts]
    if not relative or any(not (source / p).is_file() for p in relative):
        missing = [str(p) for p in relative if not (source / p).is_file()]
        raise pb.BuildStop("Pinned portable Clang input missing: " + ", ".join(missing))
    sources = []
    for path in relative:
        before = pb.identity(source / path)
        receipt.setdefault("inputs", []).append(before)
        sources.append({"path": path.as_posix(), "sha256": before["sha256"], "bytes": before["bytes"]})
    store = None
    if receipt.get("input_mode") == "thin":
        store = install_shared_compiler(source, sources, Path(os.environ.get("WORKBENCH_SHARED_COMPILER_ROOT", str(Path(os.environ["LOCALAPPDATA"]) / "PSXRecomp/toolchains"))),
                                        repair=receipt.get("repair_shared_toolchain", False))
        compiler_path = str(Path(store["root"]) / "bin/clang.exe")
        (target / "overlay_toolchain/compiler-store.sha256").write_bytes((store["content_sha256"] + "\n").encode("ascii"))
    else:
        for item in sources:
            copied = target / "overlay_toolchain/clang" / item["path"]
            copy_allowed(source / item["path"], copied)
            if pb.sha(copied) != item["sha256"]:
                raise pb.BuildStop(f"Portable compiler input changed while copying: {item['path']}")
        compiler_path = "clang/bin/clang.exe"
    (target / "overlay_toolchain/compiler.txt").write_text(compiler_path + "\n", encoding="utf-8")
    config = target / "game.toml"
    text = config.read_text(encoding="utf-8-sig")
    if any(tomllib.loads(text).get("runtime", {}).get(key) for key in
           ("overlay_autocompile_cmd", "overlay_autocompile_cmd_tcc")):
        raise pb.BuildStop("A custom overlay command cannot bypass the bundled Clang contract.")
    sections = re.split(r"(?m)(^\[[^\n]+\]\s*$)", text)
    for i in range(1, len(sections), 2):
        if sections[i].strip() == "[runtime]":
            sections[i + 1] = re.sub(r'(?m)^\s*overlay_backend\s*=.*$', '', sections[i + 1])
            sections[i + 1] = '\noverlay_backend = "gcc"\n' + sections[i + 1]
            break
    else:
        sections.append('\n[runtime]\noverlay_backend = "gcc"\n')
    updated = "".join(sections)
    if tomllib.loads(updated)["runtime"]["overlay_backend"] != "gcc":
        raise pb.BuildStop("Cannot set the optimizing Clang overlay tier.")
    config.write_text(updated, encoding="utf-8")
    receipt["overlay_compiler"] = {"kind": "shared-clang" if store else "bundled-clang", "backend": "gcc",
                                   "compiler_path": compiler_path, "store": store, "toolchain": metadata, "files": sources}


def compiler_had_nothing_to_build(runs):
    """Why the starts gave the overlay compiler no input, or None when they gave it some.

    Two cases, both read from the start gate's rows:

      * No overlay ran in any start: a reviewed kit can recompile everything its
        first minutes execute (MediEvil; Alien Trilogy).
      * Code above the kernel window was interpreted, but no start captured,
        registered or loaded an overlay: the interpreted code is boot-executable
        text the kit does not compile, an exception return into the middle of a
        function mostly (Future Cop, PS1G-57). The start gate passes that with
        the `game_code_interpreted` warning (decision 2026-10-01); the compiler
        is not involved, because a capture is its only input.

    A row from a gate that did not report the field keeps the strict rule.
    """
    if not runs:
        return None
    if not any(row.get("overlays_exercised", True) for row in runs):
        return "no overlay ran in any start"
    if all(row.get("overlays_captured") is False for row in runs):
        return "no start captured, registered or loaded an overlay"
    return None


def verify_compiler_use(product, result):
    hint = product / "overlay_toolchain/compiler.txt"
    configured = Path(hint.read_text(encoding="utf-8").strip()) if hint.exists() else Path("clang/bin/clang.exe")
    compiler = (product / "overlay_toolchain" / configured).resolve()
    logs = "\n".join(Path(row[key]).read_text(encoding="utf-8", errors="replace")
                     for row in result["runs"] for key in ("stdout", "stderr"))
    selected = [line for line in logs.splitlines() if "gcc tier using bundled toolchain" in line
                and str(compiler).replace("\\", "/") in line.replace("\\", "/")]
    shards = [p for p in (product / "cache").rglob("*.dll") if "gcc" in p.parts
              and p.with_suffix(".ranges").is_file()]
    waived = None if shards else compiler_had_nothing_to_build(result["runs"])
    if waived:
        # The compiler had nothing to build. That proves neither use nor failure;
        # record it rather than stopping the build. The runtime names its compiler
        # at every start, used or not, so that line does not decide this: with it,
        # the waiver never applied to a real product (Alien Trilogy, 2026-10-02).
        return {"compiler": str(compiler), "selection_log": selected, "fresh_shards": [],
                "exercised": any(row.get("overlays_exercised", True) for row in result["runs"]),
                "waived": waived}
    if not selected or not shards:
        raise pb.BuildStop("Moved startup did not prove bundled Clang selection and fresh optimizing-tier shards.")
    return {"compiler": str(compiler), "selection_log": selected,
            "fresh_shards": [pb.identity(p) for p in shards]}


START_FILES_FOLDER = "written-by-the-gate-starts"


def take_out_start_files(product, before, originals, receipt):
    """After the gate's starts: the payload holds what Studio staged, not what the starts wrote (PS1B-363).

    A start writes into the folder it runs from. In the payload's own folders that was, measured on the pin H
    candidate: two formatted memory cards (`saves/card1.mcd`, `saves/card2.mcd`), `saves/.pst_bios_isolated`,
    `saves/disc_digests.tsv` (one row per disc, with the disc's absolute path on the build host), an empty
    folder per BIOS under `saves/`, and `mods/state.toml`. None of it is part of a product: what a start
    creates for the player is created at the player's first start.

    `before` is `product_residue.snapshot` of the payload before the starts; `originals` holds the bytes of the
    root settings files Studio wrote. Every file the starts added inside a payload member is moved out, into
    a folder that is not a member, so it goes to the run's evidence with the other leftovers and not into the
    product. Native cache pairs are in `cache/`, which is not a member: they are promoted as before. A root
    settings file a start changed is written back as Studio wrote it: a `settings.toml` rewritten by a start
    must never ship (it could pin a boot mode). Any other staged file that changed is reported in a warning;
    the Linux build stops on that, this one does not until the fleet has shown it never happens.
    """
    product = Path(product)
    members = set(receipt["payload_members"])
    holding = product / START_FILES_FOLDER
    removed = []
    for relative in product_residue.unexpected(product, before):
        if relative.split("/", 1)[0] not in members:
            continue  # reports, captures, shard sources at the root or under cache/: never promoted
        target = holding / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        (product / relative).rename(target)
        removed.append(relative)
    # Folders the starts made inside a member and that are empty now, deepest first.
    known = set(before["folders"])
    made = [path for path in product.rglob("*") if path.is_dir() and holding not in (path, *path.parents)
            and path.relative_to(product).as_posix() not in known
            and path.relative_to(product).parts[0] in members]
    for path in sorted(made, key=lambda folder: len(folder.parts), reverse=True):
        if not any(path.iterdir()):
            path.rmdir()
            removed.append(path.relative_to(product).as_posix() + "/")
    restored, different = [], []
    for entry in product_residue.changed(product, before):
        name = entry.rsplit(" (", 1)[0]
        if name in originals:
            (product / name).write_bytes(originals[name])
            restored.append(name)
        else:
            different.append(entry)
    record = {"removed": sorted(removed), "restored": restored, "changed": different}
    receipt["start_residue"] = record
    if restored:
        receipt.setdefault("warnings", []).append(
            "A gate start rewrote " + ", ".join(restored) + "; Studio's own text was written back, so the "
            "product ships what Studio staged (PS1B-363).")
    if different:
        receipt.setdefault("warnings", []).append(
            f"A gate start changed {len(different)} file(s) the product already had: " + ", ".join(different[:6])
            + ("…" if len(different) > 6 else "") + ". They ship as the start left them; read them (PS1B-363).")
    return record


def collect_validated_payload(validated, clean, evidence, receipt):
    # Promote known runtime members, never the reports, captures or build debris
    # produced by the warmup. Keep usable native cache pairs, not shard sources.
    for name in receipt["payload_members"]:
        (validated / name).rename(clean / name)
    cache = validated / "cache"
    if cache.exists():
        for path in cache.rglob("*"):
            if path.is_file() and re.fullmatch(r"[0-9A-Fa-f]{8}_[0-9A-Fa-f]{8}\.(dll|ranges)", path.name):
                target = clean / "cache" / path.relative_to(cache)
                target.parent.mkdir(parents=True, exist_ok=True)
                path.rename(target)
    move_transients(validated, evidence / "validation-transients", receipt)


TRANSIENT_LOCKS = ("*.lock", "*.lock.*")  # the overlay cache's lock files: never evidence


def _running_from(folder):
    try:
        from . import lean_boot_gate
    except ImportError:
        import lean_boot_gate
    try:
        return lean_boot_gate.processes_under(folder)
    except Exception:  # noqa: BLE001 - naming the locker must not hide the real error
        return []


def move_transients(source, target, receipt, attempts=10, delay=1.0, sleep=time.sleep, running=_running_from):
    """Move what is left of the validated folder into the run's evidence.

    A lock can outlive the game's overlay compiler for a moment on a loaded
    host, and one locked file used to stop a build whose two starts had passed
    (WinError 33 on a cache lock file, WinError 5 on a folder; PS1B-296). Lock
    files are left out, a failed attempt is retried, and a final failure names
    whatever still runs from the folder.
    """
    source, target = Path(source), Path(target)
    error = None
    for attempt in range(attempts):
        try:
            # dirs_exist_ok: a retry continues the copy an earlier attempt began.
            shutil.copytree(source, target, dirs_exist_ok=True, ignore=shutil.ignore_patterns(*TRANSIENT_LOCKS))
            error = None
            break
        except OSError as failure:
            error = failure
            if attempt + 1 < attempts:
                sleep(delay)
    if error is not None:
        holders = ", ".join(f"{Path(image).name} (pid {pid})" for pid, image in running(source)) or "no process found"
        raise pb.BuildStop(
            f"The startup checks passed, but their leftover files could not be kept: {error}. "
            f"Still running from {source}: {holders}. Close it and build again; this stop is not a game failure.")
    if attempt:
        receipt["transients_move_retries"] = attempt
    for attempt in range(attempts):
        try:
            shutil.rmtree(source)
            return
        except OSError as failure:
            error = failure
            if attempt + 1 < attempts:
                sleep(delay)
    holders = ", ".join(f"{Path(image).name} (pid {pid})" for pid, image in running(source)) or "no process found"
    receipt.setdefault("warnings", []).append(
        f"A staging folder could not be removed after its contents were kept: {source} ({error}). "
        f"Still running from it: {holders}.")


def assert_no_netplay_state(target):
    """Refuse a product that carries one player's netplay state (PS1B-278).

    Once netplay is used, the runtime writes a device secret and a LAN room
    file beside the exe and sandboxes guest saves under saves/netplay. The
    secret is that player's lobby identity. A product is built from an
    allowlist, so none of this should be here; this keeps it that way.
    """
    found = sorted(str(path.relative_to(target)) for path in target.rglob("*")
                   if path.name.startswith("netplay_secret") or path.name == "netplay_lan_lobby.txt"
                   or (path.name == "netplay" and path.parent.name == "saves"))
    if found:
        raise pb.BuildStop("The product carries netplay player state: " + ", ".join(found) +
                           ". Rebuild from a clean run; never ship a player's lobby secret.")


def write_provenance(target, receipt):
    assert_no_netplay_state(target)
    assert_no_stray_sources(target)
    record_bundled_notice_gaps(target, receipt)
    # The bytes that ship, checked again after the startup checks and the collect step.
    staged_ui = verify_staged_ui(target, receipt)
    exe = Path(receipt["executable"]["path"]).name
    pin = receipt["pin"]
    selected_ui = receipt["ui_selection"]
    info = {"schema": 2, "build_route": "workbench_private_lean", "platform": "windows-x64",
            "runtime_commit": pin["commit"],
            "runtime_tree": pin["tree"], "ui_commit": selected_ui["ui"]["commit"],
            "ui_tree": selected_ui["ui"].get("tree"), "ui_selection": selected_ui["mode"],
            "launcher_module": selected_ui["module"],
            "shared_sdl": selected_ui["shared_sdl"],
            "ui_assets": selected_ui["assets"],
            "ui_staged_identity": staged_ui,
            "toolchain": pin["toolchain_pin"], "pin_snapshot": receipt.get("pin_snapshot"),
            "serials": [d["serial"] for d in receipt["disc_identities"]],
            "input_mode": receipt["input_mode"], "executable": exe,
            "executable_sha256": pb.sha(target / exe), "lean_recipe": receipt["lean_recipe"],
            "lean_compile": receipt["lean_compile"],
            "netplay": receipt["lean_compile"].get("netplay", "off"),
            "framework_pin_stamped": receipt["lean_compile"].get("framework_pin"),
            "lobby_version": receipt["lean_compile"].get("lobby_version"),
            "optimize_pgo": receipt.get("optimize_pgo", False),
            "pgo_result": receipt.get("pgo_result"), "runtime_settings": receipt.get("runtime_settings"),
            "overlay_compiler": receipt.get("overlay_compiler"),
            "boot_gate": receipt.get("private_validation"), "full_game_correctness": "NOT_CLAIMED",
            "kit": receipt.get("kit"), "kit_review": receipt.get("kit_review"),
            "payload": pb.inventory(target)}
    if receipt.get("ui_module_build"):
        # PS1B-482: what the launcher-module build added beside the executable.
        info.update(ui_module_build="launcher-module", ui_asset_receipt=receipt.get("ui_asset_receipt"),
                    module_runtime_dlls=receipt.get("module_runtime_dlls"))
    pb.write_json(target / "BUILDINFO.json", info)
    sums = pb.inventory(target)
    (target / "SHA256SUMS.txt").write_text("".join(f"{item['sha256']}  {item['path']}\n" for item in sums), encoding="utf-8")


def product_roster(product):
    """([disc paths], [serials]) of the product's game.toml; empty lists when it has none."""
    try:
        game = tomllib.loads((Path(product) / "game.toml").read_text(encoding="utf-8-sig")).get("game", {})
    except (OSError, ValueError):
        return [], []
    discs = game.get("discs") or ([game["disc"]] if game.get("disc") else [])
    serials = game.get("disc_serials") or []
    return list(discs), [s if isinstance(s, str) else None for s in serials]


def other_discs(product):
    """The discs after the first, for the boot gate: [{'disc': N, 'serial': …}].

    The gate's two standard starts use disc 1. A multi-disc product must also
    start from each other disc of its roster (PS1B-333).
    """
    discs, serials = product_roster(product)
    return [{"disc": n, "serial": serials[n - 1] if n <= len(serials) and serials[n - 1] else None}
            for n in range(2, len(discs) + 1)]


def first_disc_serial(product):
    serials = product_roster(product)[1]
    return serials[0] if serials else None


def validate_payload(work, product, evidence, receipt):
    try:
        from . import lean_boot_gate
    except ImportError:
        import lean_boot_gate
    parked = work.with_name("parked-development-work")
    if parked.exists() or work.resolve().parent != evidence.resolve().parent:
        raise pb.BuildStop("Cannot safely park the owned workspace for the runtime-only check.")
    launch_cwd = evidence / "unrelated-cwd"
    launch_cwd.mkdir()
    if (product / "cache").exists():
        raise pb.BuildStop("The private startup gate requires an empty initial native cache.")
    work.rename(parked)
    receipt["game_launched"] = True
    def update(row, status):
        label = f"Private startup, disc {row['disc']}" if "disc" in row else f"Private startup {row['number']}"
        pb.progress.stage(receipt, evidence, f"private-boot-{row['number']}", status, label=label, log=row["stdout"])
    try:
        with pb.progress.measured(receipt, evidence, "private-validation", "Validate moved playable folder"):
            # What the payload holds before any start, and Studio's own root settings files (PS1B-363).
            staged = product_residue.snapshot(product)
            originals = {name: (product / name).read_bytes() for name in RUNTIME_FILES if (product / name).is_file()}
            extra = other_discs(product)
            result = lean_boot_gate.run_lean_boots(product, Path(receipt["executable"]["path"]).name,
                evidence / "private-boots", launch_cwd, os.environ.copy(), on_run=update,
                **({"discs": extra} if extra else {}))
            receipt["private_validation"] = result
            if result.get("disc_runs"):
                # Every disc of the roster started, not only the remembered one (PS1B-333).
                receipt["disc_boot_check"] = (
                    [{"disc": 1, "serial": first_disc_serial(product), "frames": result["runs"][-1].get("frames"),
                      "passed": True}]
                    + [{"disc": row["disc"], "serial": row.get("mounted_serial"), "frames": row.get("frames"),
                        "passed": bool(row.get("passed"))} for row in result["disc_runs"]])
            if receipt.get("overlay_compiler", {}).get("kind") in ("bundled-clang", "shared-clang"):
                receipt["overlay_compiler"]["validation"] = verify_compiler_use(product, result)
            # Does every overlay the starts captured compile? The runtime does not say (PS1B-380), so the
            # product's own driver is asked, on this validation copy. A warning, never a stop.
            with pb.progress.measured(receipt, evidence, "overlay-compile-check", "Check that captured overlays compile"):
                overlay_compile_check.record_in(receipt, overlay_compile_check.run(
                    product, evidence, jobs=os.environ.get("WORKBENCH_BUILD_JOBS") or None))
            # Last: the checks above read what the starts left (captures, cache). The product does not get it.
            take_out_start_files(product, staged, originals, receipt)
            receipt["disp_native"] = result["runs"][-1]["disp_native"]
            # What the wave summary lists from the second start: interpreted game code
            # (PS1B-266) and the CD read counters. Warnings, never stops.
            lean_boot_gate.record_start_findings(result["runs"][-1], receipt)
            # The heaviest process of any start, and the limit the starts ran under (PS1B-352).
            lean_boot_gate.record_start_memory(result["runs"] + result.get("disc_runs", []), receipt)
            # What each start says about its own overlay compiles, when its runtime reports it (PS1B-380).
            lean_boot_gate.record_overlay_compile(result["runs"] + result.get("disc_runs", []), receipt)
            receipt["native_dispatch_note"] = "Measured on second moved private start with development workspace unavailable."
    finally:
        # Restore the retained failure workspace as well as the successful one.
        parked.rename(work)


def check_registry(original, snapshot, receipt):
    """Stop only when the live pin's build identity moved during the build.

    PIN.json also carries living metadata (reviews, known_issues, test notes)
    that the integrator edits while builds run. Those edits are recorded, not
    fatal. A status change stops the build only when the pin is no longer
    buildable, so a test -> qualified promotion does not abort it.
    """
    try:
        now = original.read_bytes()
        after = json.loads(now.decode("utf-8-sig"))
    except (OSError, ValueError) as error:
        raise pb.BuildStop(f"The selected live pin can no longer be read ({error}); "
                           "the run-bound snapshot is retained.") from error
    if now == snapshot:
        return
    before = json.loads(snapshot.decode("utf-8-sig"))
    receipt["pin_registry_after"] = pb.identity(original)
    changed = [k for k in pb.PIN_IDENTITY if before.get(k) != after.get(k)]
    if after.get("status") not in pb.PIN_BUILDABLE:
        changed.append("status")
    if changed:
        raise pb.BuildStop("The selected live pin changed its build identity during this build "
                           f"({', '.join(changed)}); the run-bound snapshot is retained.")
    notes = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    receipt["pin_metadata_changed"] = notes
    warning = (f"Live pin metadata changed during the build ({', '.join(notes) or 'formatting only'}); "
               "the build identity still matches the retained run-bound pin snapshot.")
    if warning not in receipt.setdefault("warnings", []):
        receipt["warnings"].append(warning)


def retain_failed_payloads(folders, evidence, receipt):
    """Move a failed build's staging folders out of the operator's output root.

    Staging sits beside the output so the final promotion is one same-volume
    rename. After a stop nothing is promoted, so each folder moves under the
    run's evidence instead of staying there as a hidden ".<name> moved
    validation <id>" folder (PS1B-256). A folder that cannot be moved (a
    lingering game still holds a file) stays where it is and is named.
    """
    root, left = evidence / "failed-payload", []
    for label, folder in folders.items():
        if folder is None or not folder.exists():
            continue
        root.mkdir(exist_ok=True)
        try:
            shutil.move(str(folder), str(root / label))
        except OSError as error:
            left.append(f"{folder} ({error})")
    if root.exists():
        receipt["failed_payload"] = str(root)
    if left:
        receipt.setdefault("warnings", []).append(
            "A failed build's staging folder could not be moved to its evidence and was left in place: "
            + "; ".join(left))
        receipt["failed_payload_left"] = left


def build(args):
    """One build, or one build per program when the discs are the programs of a set.

    The single build finds out: its kit match stops with a plan when the selected
    discs match several kits that declare one set (PS1B-333). That planning run
    keeps its evidence as a stopped run; the set build then builds each program
    and joins them (workbench_set).
    """
    receipt = build_single(args)
    plan = receipt.get("program_set")
    if receipt.get("status") != "human-stop" or not plan or getattr(args, "kit", None):
        return receipt
    try:
        from . import workbench_set
    except ImportError:
        import workbench_set
    return workbench_set.build(args, plan, build_single, plan_receipt=receipt)


def build_single(args):
    destination = args.output.resolve()
    if destination.exists() and any(destination.iterdir()):
        raise pb.BuildStop("The output directory is occupied. Select a new empty directory.")
    if getattr(args, "output_kind", "private") == "setup" and getattr(args, "optimize_pgo", False):
        raise pb.BuildStop("PGO is a private product option; it cannot be applied to an inputs-free setup package.")
    cache = runs.run_root(getattr(args, "run_root", None))
    free_gb = runs.require_free_space(cache)
    destination.mkdir(parents=True, exist_ok=True)
    reservation = destination / ".workbench-reservation"
    with reservation.open("x", encoding="utf-8"):
        pass
    cache.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=cache))
    runs.claim(run, destination)  # which process builds this run (PS1B-351)
    work, evidence = run / "work", run / "evidence"
    evidence.mkdir()
    receipt = {"status": "started", "commands": [], "inputs": [], "warnings": [],
               "stage_history": str(cache / "workbench-stage-history.json")}
    staged = None
    pending = {}  # staging folders beside the output, by role, until promotion
    try:
        pin_bytes = args.pin.read_bytes()
        snapshot = evidence / "PIN.json"
        snapshot.write_bytes(pin_bytes)
        local = copy(args)
        local.output, local.pin = work, snapshot
        local.stage_history = cache / "workbench-stage-history.json"
        receipt = pb._build_workspace(local)
        receipt.update(output=str(destination), receipt=str(evidence / "build-receipt.json"), logs=str(evidence),
                       work_directory=str(work), work_retained=True, keep_development_files=bool(getattr(args, "keep_development_files", False)),
                       pin_snapshot=pb.identity(snapshot), pin_registry_selected={"path": str(args.pin.resolve()),
                           "bytes": len(pin_bytes), "sha256": hashlib.sha256(pin_bytes).hexdigest()})
        for path in work.glob("*.log"):
            shutil.copy2(path, evidence / path.name)
        if (work / "build-receipt.json").is_file():
            shutil.copy2(work / "build-receipt.json", evidence / "workspace-build-receipt.json")
        for row in [*receipt.get("commands", []), *receipt.get("stages", [])]:
            if row.get("log") and (evidence / Path(row["log"]).name).is_file():
                row["log"] = str(evidence / Path(row["log"]).name)
            if "stop_file" in row:
                row["stop_file"] = str(evidence / "HUMAN-STOP.txt")
        if receipt["status"] == "human-stop":
            raise pb.BuildStop(receipt["next_action"])
        with pb.progress.measured(receipt, evidence, "pin-snapshot-check", "Verify run-bound pin"):
            check_registry(args.pin, pin_bytes, receipt)
        staged = Path(tempfile.mkdtemp(prefix=f".{destination.name} moved validation ", dir=destination.parent))
        pending["validation"] = staged
        if receipt["output_kind"] == "private":
            with pb.progress.measured(receipt, evidence, "stage-payload", "Assemble playable folder"):
                stage_payload(work, staged, receipt)
            if receipt.get("startup_checks", True):
                validate_payload(work, staged, evidence, receipt)
            else:
                receipt["private_validation"] = {"status": "not-requested", "scope": "public-edition player build"}
            with pb.progress.measured(receipt, evidence, "final-input-integrity", "Verify inputs after startup checks"):
                pb.check_inputs(receipt)
                check_registry(args.pin, pin_bytes, receipt)
            # Each platform gets its own self-contained folder under the output.
            clean = Path(tempfile.mkdtemp(prefix=f".{destination.name} ready ", dir=destination.parent))
            pending["ready"] = clean
            (clean / "windows-x64").mkdir()
            collect_validated_payload(staged, clean / "windows-x64", evidence, receipt)
            pending.pop("validation")
            staged = clean
            write_provenance(staged / "windows-x64", receipt)
            if "linux-x64" in receipt.get("build_platforms", []):
                try:
                    from . import linux_build
                except ImportError:
                    import linux_build
                with pb.progress.measured(receipt, evidence, "linux-build", "Build and check Linux x64"):
                    linux_build.build(work, staged / "windows-x64", staged / "linux-x64",
                                      evidence, receipt, run.name)
                linux_build.write_provenance(staged / "linux-x64", receipt)
            if "macos-x64" in receipt.get("build_platforms", []):
                try:
                    from . import mac_build
                except ImportError:
                    import mac_build
                with pb.progress.measured(receipt, evidence, "mac-build", "Build macOS x64 and check it on Pacifica"):
                    mac_build.build(work, staged / "windows-x64", staged / "macos-x64",
                                    evidence, receipt, run.name)
                mac_build.write_provenance(staged / "macos-x64", receipt)
            receipt["qualification"] = ("Native build and moved headless starts passed; no TAS or whole-game qualification."
                if receipt.get("startup_checks", True) else "Native build and packaging only; startup, gameplay and replay not tested.")
        else:
            archive = Path(receipt["windows_package"]["path"])
            shutil.copy2(archive, staged / archive.name)
        reservation.unlink()
        destination.rmdir()  # Only our verified empty, reserved output directory.
        staged.rename(destination)
        staged = None
        pending.clear()
        if receipt["output_kind"] == "private":
            receipt["executable"] = pb.identity(destination / "windows-x64" / Path(receipt["executable"]["path"]).name)
            receipt["runtime_config"] = str(destination / "windows-x64" / "game.toml")
            for key, folder in (("linux", "linux-x64"), ("macos", "macos-x64")):
                if receipt.get(key):
                    receipt[key]["archive_path"] = str(destination / folder / receipt[key]["archive"]["name"])
        else:
            receipt["windows_package"] = pb.identity(destination / Path(receipt["windows_package"]["path"]).name)
        receipt["outputs"] = pb.inventory(destination)
        if receipt.get("windows_package") and receipt["output_kind"] == "private":
            archive = Path(receipt["windows_package"]["path"])
            retained = evidence / archive.name
            shutil.copy2(archive, retained)
            receipt["windows_package"] = pb.identity(retained)
        if not receipt["keep_development_files"]:
            if work.resolve().parent != run.resolve() or run.resolve().parent != cache.resolve():
                raise pb.BuildStop("Refusing cleanup outside this run's owned workspace.")
            import delivery
            delivery.remove_owned_tree(work, run)
            receipt["work_retained"] = False
    except Exception as error:
        receipt.update(status="human-stop", next_action=str(error), output=str(destination),
                       receipt=str(evidence / "build-receipt.json"), logs=str(evidence),
                       work_directory=str(work), work_retained=work.exists())
        if not receipt.get("failed_stage"):
            pb.progress.stage(receipt, evidence, "output-finalize", "failed", detail=str(error))
        (evidence / "HUMAN-STOP.txt").write_text(str(error)+"\n", encoding="utf-8")
        receipt["stop_file"] = str(evidence / "HUMAN-STOP.txt")
        retain_failed_payloads(pending, evidence, receipt)
    finally:
        reservation.unlink(missing_ok=True)
        pb.progress.finish(receipt, evidence, receipt["status"] == "human-stop", receipt.get("next_action"))
        receipt["run_root"] = {"path": str(cache), "free_gb_at_start": free_gb}
        receipt["host"] = socket.gethostname()  # two build hosts feed one wave report
        # The receipt must exist before retention looks at this run, and must
        # record what retention did: write it, prune, write it again.
        pb.write_json(evidence / "build-receipt.json", receipt)
        runs.after_build(cache, run, evidence, receipt)
        pb.write_json(evidence / "build-receipt.json", receipt)
    return receipt
