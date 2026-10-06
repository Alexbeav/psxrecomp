#!/usr/bin/env python3
"""The BIOS emitter fingerprint must hash what psxrecomp-bios is built from (PS1B-131).

tools/bios_emitter_fingerprint.sh takes its file list from
add_executable(psxrecomp-bios ...) and from the include lines of those
sources. This test holds the script to that, in both directions, with a
second reading of the same files written here in Python:

  1. Its sources are the sources of the target: none missing, none extra.
     CMake passes its own list of the target's sources (--target-sources),
     so a target that the script's reading cannot follow fails here.
  2. Its headers are the quoted includes of those sources, followed through
     the repository: none missing, none extra.
  3. On a copy of the tree: a change to any kind of input moves the
     fingerprint, a change to a file the target does not compile leaves it,
     a source added to the target is hashed without an edit of the script,
     and a target the script cannot read is an error, not an empty list.
  4. The hand-kept list of pin H is still computed on request, to the value
     pin H gave. runtime/bios_stale_check.cmake uses it so that a stamp
     written before the list changed is not reported as a stale BIOS, while
     a real change is.

Every part prints what it examined.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = "tools/bios_emitter_fingerprint.sh"
CMAKE_LISTS = "recompiler/CMakeLists.txt"
STALE_CHECK = ROOT / "runtime" / "bios_stale_check.cmake"
TARGET = "psxrecomp-bios"
INCLUDE_DIRS = ("recompiler/include", "recompiler/src")
SKIP = 77

# The hand-kept list as pin H had it. Three of its files are not sources of
# the target; three sources of the target are not in it.
HAND_LIST = (
    "recompiler/src/full_function_emitter.cpp", "recompiler/src/full_function_emitter.h",
    "recompiler/src/strict_translator.cpp", "recompiler/src/main_bios.cpp",
    "recompiler/src/function_discovery.cpp", "recompiler/src/bios_address_model.cpp",
    "recompiler/src/bios_address_model.h", "recompiler/src/config_loader.cpp",
    "recompiler/src/control_flow.cpp", "recompiler/src/function_analysis.cpp",
    "recompiler/src/mips_decoder.cpp", "recompiler/src/bios_slice_walker.cpp",
    "recompiler/src/basic_block.cpp", "runtime/include/psx_cyc.h",
    "runtime/include/psx_instr_cost.h",
)
WAS_BLIND = ("recompiler/src/pgxp_hook_emitter.cpp", "recompiler/src/ps1_exe_parser.cpp",
             "recompiler/src/recompiler_patch.cpp")
WAS_FALSE = ("recompiler/src/basic_block.cpp", "recompiler/src/control_flow.cpp",
             "recompiler/src/function_analysis.cpp")


def find_bash() -> str | None:
    """A bash for the script. On Windows never a WSL launcher (System32, WindowsApps)."""
    if os.name != "nt":
        return shutil.which("bash")
    candidates = []
    for name in ("ProgramFiles", "ProgramW6432"):
        base = os.environ.get(name)
        if base:
            candidates += [os.path.join(base, "Git", "bin", "bash.exe"),
                           os.path.join(base, "Git", "usr", "bin", "bash.exe")]
    git = shutil.which("git")
    if git:
        parents = Path(git).resolve().parents
        for up in (1, 2):
            if up < len(parents):
                candidates += [str(parents[up] / "bin" / "bash.exe"),
                               str(parents[up] / "usr" / "bin" / "bash.exe")]
    candidates += [os.path.join(entry, "bash.exe")
                   for entry in os.environ.get("PATH", "").split(os.pathsep) if entry]
    system_root = os.path.normcase(os.path.abspath(os.environ.get("SystemRoot") or r"C:\Windows"))
    for candidate in candidates:
        folded = os.path.normcase(os.path.abspath(candidate))
        if folded.startswith(system_root + os.sep) or (os.sep + "windowsapps" + os.sep) in folded:
            continue
        if os.path.isfile(candidate):
            return candidate
    return None


def shell_env(bash: str) -> dict:
    """PATH that reaches the tools beside `bash` (awk, sed, sha256sum) before any other."""
    env = dict(os.environ)
    here = os.path.dirname(bash)
    extra = [here, os.path.join(os.path.dirname(here), "bin"),
             os.path.join(os.path.dirname(here), "usr", "bin")]
    env["PATH"] = os.pathsep.join([p for p in extra if os.path.isdir(p)] + [env.get("PATH", "")])
    return env


class Tree:
    """One tree with the script in it: the checkout, or a copy of the files it needs."""

    def __init__(self, root: Path, bash: str):
        self.root = root
        self.bash = bash
        self.env = shell_env(bash)

    def run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.bash, str(self.root / SCRIPT), *args], cwd=self.root, env=self.env,
            capture_output=True, text=True, encoding="utf-8", errors="replace")

    def fingerprint(self, *args: str) -> str:
        result = self.run(*args)
        assert result.returncode == 0, (args, result.returncode, result.stderr)
        value = result.stdout.strip()
        assert re.fullmatch(r"[0-9a-f]{64}", value), (args, result.stdout, result.stderr)
        return value

    def listing(self, *args: str) -> list[tuple[str, str, str]]:
        result = self.run("--list", *args)
        assert result.returncode == 0, (args, result.returncode, result.stderr)
        rows = [tuple(line.split("\t")) for line in result.stdout.splitlines() if line]
        assert rows and all(len(row) == 3 for row in rows), result.stdout
        return rows

    def of_kind(self, kind: str, *args: str) -> list[str]:
        return [path for row_kind, _, path in self.listing(*args) if row_kind == kind]


def cmake_target_sources(root: Path) -> list[str]:
    """Second reading of add_executable(psxrecomp-bios ...), paths from the repository root."""
    text = (root / CMAKE_LISTS).read_text(encoding="utf-8")
    text = re.sub(r"#[^\n]*", "", text)
    block = re.search(r"add_executable\s*\(\s*" + re.escape(TARGET) + r"(?=[\s)])([^)]*)\)", text)
    assert block, f"{CMAKE_LISTS} has no add_executable({TARGET} ...)"
    return [os.path.normpath("recompiler/" + word).replace(os.sep, "/") for word in block.group(1).split()]


def include_closure(root: Path, sources: list[str]) -> list[str]:
    """Second reading of the headers: quoted includes, followed through the repository."""
    seen = list(sources)
    headers = []
    for file in seen:                      # grows while it is walked
        text = (root / file).read_text(encoding="utf-8", errors="replace")
        for name in re.findall(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', text, re.M):
            for base in (os.path.dirname(file), *INCLUDE_DIRS):
                candidate = os.path.normpath(os.path.join(base, name)).replace(os.sep, "/")
                if candidate.startswith(".."):
                    continue
                if (root / candidate).is_file():
                    if candidate not in seen:
                        seen.append(candidate)
                        headers.append(candidate)
                    break
    return sorted(headers)


def two_way(what: str, wanted: list[str], hashed: list[str]) -> None:
    """Fail unless the two lists hold the same files, and print both sides."""
    missing = sorted(set(wanted) - set(hashed))
    extra = sorted(set(hashed) - set(wanted))
    print(f"{what}: {len(wanted)} wanted, {len(hashed)} fingerprinted, "
          f"{len(missing)} not fingerprinted, {len(extra)} fingerprinted without cause")
    assert wanted, f"{what}: the wanted list is empty; nothing was examined"
    assert not missing and not extra, (
        f"{what}: the fingerprint list and its source disagree\n"
        f"  in the source, not fingerprinted: {missing}\n"
        f"  fingerprinted, not in the source: {extra}")
    assert len(set(hashed)) == len(hashed), f"{what}: a file is fingerprinted twice: {hashed}"


def digest_of_digests(root: Path, files: list[str], line: str) -> str:
    """The script's digest, computed here: one line for each file that exists."""
    lines = "".join(line.format(hashlib.sha256((root / file).read_bytes()).hexdigest())
                    for file in files if (root / file).is_file())
    return hashlib.sha256(lines.encode()).hexdigest()


def check_lists(tree: Tree, target_sources_from_cmake: str | None) -> None:
    target = cmake_target_sources(tree.root)
    if target_sources_from_cmake is not None:
        from_cmake = [os.path.normpath("recompiler/" + word).replace(os.sep, "/")
                      for word in target_sources_from_cmake.split(",") if word]
        two_way("target sources: CMake's own list against this test's reading", from_cmake, target)
        target = from_cmake
    else:
        print("target sources: read from the CMake text only (no --target-sources)")
    sources = tree.of_kind("source", "bios/OpenBIOS.toml")
    two_way("emitter sources: psxrecomp-bios target against the fingerprint", target, sources)
    assert sources == target, f"the sources are not hashed in the target's order: {sources}"

    headers = tree.of_kind("header", "bios/OpenBIOS.toml")
    two_way("emitter headers: include lines against the fingerprint",
            include_closure(tree.root, target), headers)

    rows = tree.listing("bios/OpenBIOS.toml")
    absent = [path for kind, state, path in rows if kind != "profile" and state != "present"]
    assert not absent, f"the fingerprint names emitter files that do not exist: {absent}"
    kinds = {kind for kind, _, _ in rows}
    assert kinds == {"source", "header", "extra", "profile"}, kinds
    for name in WAS_BLIND:
        assert name in sources, f"{name} is a source of the target and is not fingerprinted"
    for name in WAS_FALSE:
        assert name not in [path for _, _, path in rows], f"{name} is fingerprinted and is not in the target"


def copy_tree(source: Tree, dest: Path) -> Tree:
    """The files the script reads, and the three it must not read, in a folder of their own."""
    emitter = [path for kind, _, path in source.listing("bios/OpenBIOS.toml") if kind != "profile"]
    for name in [SCRIPT, CMAKE_LISTS, *emitter, *HAND_LIST]:
        if (source.root / name).is_file():
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source.root / name, dest / name)
    (dest / "bios").mkdir()
    (dest / "bios" / "T.toml").write_text(
        '[recompiler]\nrom = "bios/T.BIN"\nseeds = "bios/T-seeds.json"\nout_stem = "T"\n',
        encoding="utf-8", newline="\n")
    (dest / "bios" / "T.BIN").write_bytes(b"rom image")
    (dest / "bios" / "T-seeds.json").write_bytes(b'{"seeds": []}\n')
    return Tree(dest, source.bash)


def touch(path: Path) -> bytes:
    """Add one byte; return the original bytes for restore()."""
    original = path.read_bytes()
    path.write_bytes(original + b"\n")
    return original


def check_behaviour(copy: Tree) -> None:
    profile = "bios/T.toml"
    base = copy.fingerprint(profile)
    runs = 1

    # The value is the digest of one digest line for each file, and of nothing else.
    listed = [path for _, _, path in copy.listing(profile)]
    assert base == digest_of_digests(copy.root, listed, "{}\n"), "the digest is not the documented one"
    # The profile's spelling is not part of it.
    assert copy.fingerprint(str(copy.root / profile)) == base, "an absolute profile path moves the fingerprint"
    runs += 1

    # One file of each kind moves it; so does each source the old list did not see.
    moving = [*WAS_BLIND, "recompiler/src/load_value_emission.h", "runtime/include/psx_cyc.h",
              profile, "bios/T.BIN", "bios/T-seeds.json"]
    for name in moving:
        original = touch(copy.root / name)
        assert copy.fingerprint(profile) != base, f"a change to {name} does not move the fingerprint"
        (copy.root / name).write_bytes(original)
        runs += 1
    assert copy.fingerprint(profile) == base
    # The three files the target does not compile do not move it.
    originals = {name: touch(copy.root / name) for name in WAS_FALSE if (copy.root / name).is_file()}
    assert originals, "none of the three former false positives is in this tree"
    assert copy.fingerprint(profile) == base, f"a change to {sorted(originals)} moves the fingerprint"
    for name, original in originals.items():
        (copy.root / name).write_bytes(original)
    runs += 2
    print(f"behaviour: {len(moving)} inputs move the fingerprint, {len(originals)} files outside the target do not")

    # The hand list of pin H, on request, to pin H's value. sha256sum marks a
    # stdin digest "  -" or " *-" by platform; pin H hashed that line as it came.
    hand_files = [*HAND_LIST, profile, "bios/T-seeds.json", "bios/T.BIN"]
    hand = copy.fingerprint("--hand-list", profile)
    assert hand in (digest_of_digests(copy.root, hand_files, "{}  -\n"),
                    digest_of_digests(copy.root, hand_files, "{} *-\n")), "--hand-list is not pin H's value"
    assert hand != base
    assert copy.of_kind("hand", "--hand-list", profile) == list(HAND_LIST)
    runs += 2

    # A source added to the target is hashed with no edit of the script.
    cmake = copy.root / CMAKE_LISTS
    original_cmake = cmake.read_text(encoding="utf-8")
    opening = re.search(r"add_executable\s*\(\s*" + re.escape(TARGET) + r"[ \t]*\n", original_cmake)
    assert opening, "the copy's CMake text has no psxrecomp-bios block to extend"
    (copy.root / "recompiler/src/added_in_test_9.cpp").write_text('#include "added_in_test_9.h"\n', encoding="utf-8")
    (copy.root / "recompiler/src/added_in_test_9.h").write_text("// new header\n", encoding="utf-8")
    cmake.write_text(original_cmake[:opening.end()] + "    src/added_in_test_9.cpp  # with a comment\n"
                     + original_cmake[opening.end():], encoding="utf-8", newline="\n")
    assert "recompiler/src/added_in_test_9.cpp" in copy.of_kind("source", profile)
    assert "recompiler/src/added_in_test_9.h" in copy.of_kind("header", profile)
    assert copy.fingerprint(profile) != base, "a source added to the target is not hashed"
    runs += 3

    # A target the script cannot read is an error. An empty list would hash
    # the profile alone and call every emitter change fresh.
    for label, text in (
            ("a variable in the target", original_cmake[:opening.end()] + "    ${BIOS_SOURCES}\n" + original_cmake[opening.end():]),
            ("no target", original_cmake.replace("add_executable(" + TARGET, "add_executable(some-other-tool"))):
        cmake.write_text(text, encoding="utf-8", newline="\n")
        result = copy.run(profile)
        assert result.returncode == 3 and not result.stdout.strip(), (label, result.returncode, result.stdout)
        assert "bios_emitter_fingerprint:" in result.stderr, (label, result.stderr)
        runs += 1
    # Without the recompiler's CMake file there are no emitter sources to
    # hash (an install with built emitters): the profile inputs are the list.
    cmake.unlink()
    assert {kind for kind, _, _ in copy.listing(profile)} == {"profile"}
    assert copy.fingerprint(profile) == digest_of_digests(
        copy.root, [profile, "bios/T-seeds.json", "bios/T.BIN"], "{}\n")
    cmake.write_text(original_cmake, encoding="utf-8", newline="\n")
    assert copy.fingerprint(profile) == base
    runs += 3
    print(f"behaviour: {runs} script runs on the copy; added source hashed; unreadable target refused")


def check_stale_verdicts(copy: Tree, cmake: str | None) -> None:
    """The configure-time check, run alone: which stamps are STALE and which are not."""
    cmake = cmake or shutil.which("cmake")
    if not cmake:
        print("stale check verdicts: NOT RUN (no cmake on this host and no --cmake)")
        return
    profile = copy.root / "bios" / "T.toml"
    stamp = copy.root / "generated" / "T.emitter.sha"
    stamp.parent.mkdir(exist_ok=True)

    def verdict() -> str:
        result = subprocess.run(
            [cmake, "-DPSXRECOMP_BIOS_STALE_CHECK_RUN=ON", f"-DPSXRECOMP_ROOT={copy.root.as_posix()}",
             "-DPSXRECOMP_BIOS_STEM=T", f"-DPSXRECOMP_BIOS_PROFILE={profile.as_posix()}",
             f"-D_psxrt_bash={Path(copy.bash).as_posix()}", "-P", str(STALE_CHECK)],
            cwd=copy.root, env=copy.env, capture_output=True, text=True, encoding="utf-8", errors="replace")
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        text = result.stdout + result.stderr
        if "is STALE" in text:
            return "stale"
        if "earlier file list" in text:
            return "earlier list, not stale"
        if "carries no emitter" in text:
            return "no stamp"
        return "fresh"

    current = copy.fingerprint(str(profile))
    hand = copy.fingerprint("--hand-list", str(profile))
    seen = []

    def expect(case: str, wanted: str) -> None:
        got = verdict()
        seen.append(f"{case}: {got}")
        assert got == wanted, f"{case}: the check says '{got}', wanted '{wanted}'"

    stamp.unlink(missing_ok=True)
    expect("no stamp", "no stamp")
    stamp.write_text(current + "\n", encoding="utf-8")
    expect("stamp of this tree", "fresh")
    # A tree stamped at pin H, nothing changed: only the list moved.
    stamp.write_text(hand + "\n", encoding="utf-8")
    expect("stamp of the pin H list, tree unchanged", "earlier list, not stale")
    # The same stamp after a change to a file the pin H list covered: really stale.
    original = touch(copy.root / "recompiler/src/full_function_emitter.cpp")
    expect("stamp of the pin H list, a covered file changed", "stale")
    (copy.root / "recompiler/src/full_function_emitter.cpp").write_bytes(original)
    # A current stamp after a change to a source the pin H list did not see.
    stamp.write_text(current + "\n", encoding="utf-8")
    original = touch(copy.root / "recompiler/src/ps1_exe_parser.cpp")
    expect("stamp of this tree, ps1_exe_parser.cpp changed", "stale")
    (copy.root / "recompiler/src/ps1_exe_parser.cpp").write_bytes(original)
    stamp.write_text("0" * 64 + "\n", encoding="utf-8")
    expect("a stamp of neither list", "stale")
    print("stale check verdicts: " + "; ".join(seen))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-sources",
                        help="the SOURCES of psxrecomp-bios as CMake holds them, joined by commas")
    parser.add_argument("--cmake", help="the cmake program, for the stale check's verdicts")
    args = parser.parse_args()

    bash = find_bash()
    if bash is None:
        print("bios emitter fingerprint test: SKIP (no bash on this host; the fingerprint "
              "script cannot run here, and the build skips its staleness check too)")
        return SKIP
    print(f"bash: {bash}")
    tree = Tree(ROOT, bash)
    check_lists(tree, args.target_sources)
    with tempfile.TemporaryDirectory(prefix="psx-emitter-fp-") as tmp:
        copy = copy_tree(tree, Path(tmp))
        check_behaviour(copy)
        check_stale_verdicts(copy, args.cmake)
    print("bios emitter fingerprint test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
