"""PS1B-566: the setup host unpacks the toolchain zip on a host without unzip.

The POSIX host ran `unzip`, then `tar -xf`. Many Linux hosts have no unzip and
GNU tar cannot read a zip, so a player there could not install the toolchain
("sh: 1: unzip: not found / tar: This does not look like a tar archive").

Each case runs the host's `install` in the closed environment of
toolchain_test_support. PATH is one folder of links to every program in the
closed PATH (/usr/bin:/bin) except the programs that can unpack a zip
(unzip, bsdtar, tar, python3, cmake), plus a link to the one extractor under
test. So the machine's own unzip, and the `tar` that bsdtar provides on macOS
and the BSDs, are hidden; the pack's check still finds rm and the rest:
  - python3 only: the pack is installed, its programs still executable;
  - each of unzip, bsdtar and cmake that this host has: the same (a host
    without one prints a SKIP line for it);
  - nothing: the install is refused, the sentence names the extractors it
    looked for, and the installed pack is left as it was.

Exit 0 pass, 1 fail, 77 skipped (Windows, where the host uses tar.exe; or no
recomp-ui or C compiler to build the host probe).
"""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402

SKIPPED = 77
SENTENCE = "none of unzip, bsdtar, python3 or cmake is on PATH"


SYSTEM_PATH = ("/usr/bin", "/bin")     # the PATH of support.closed_environment()
EXTRACTORS = ("unzip", "bsdtar", "tar", "gtar", "python3", "cmake")


def hides(name: str) -> bool:
    return name in EXTRACTORS or name.startswith("python3") or name.startswith("unzip")


def only_on_path(sandbox: Path, name: str, program: str | None) -> str:
    """A folder of links to the closed PATH's programs without the zip extractors, plus
    `program` linked as `name` (with None, no extractor at all)."""
    folder = sandbox / "path-only"
    folder.mkdir(parents=True)
    for system in SYSTEM_PATH:
        for entry in sorted(Path(system).iterdir()):
            target = folder / entry.name
            if hides(entry.name) or target.exists() or target.is_symlink():
                continue
            if entry.is_file() and os.access(entry, os.X_OK):
                target.symlink_to(entry)
    if program:
        (folder / name).symlink_to(program)
    return str(folder)


def install_with(probe: Path, tmp: Path, stub: Path, case: str, name: str, program: str | None) -> dict:
    sandbox = tmp / ("zip-" + case)
    env = support.closed_environment(sandbox)
    cache = Path(env["RETCOMM_TOOLCHAIN_CACHE"])
    support.make_pack(cache / "1.0.14", marker="old.txt", stub=stub)
    archive = support.make_zip(sandbox / "pack.zip", stub=stub)
    seen = support.run_probe(probe, ["install", str(archive)], sandbox, False,
                             {"PATH": only_on_path(sandbox, name, program)})
    pack = cache / "1.0.14"
    cmake = pack / "bin" / support.CMAKE
    seen["new"] = (pack / "new.txt").is_file()
    seen["old"] = (pack / "old.txt").is_file()
    seen["cmake_executable"] = cmake.is_file() and os.access(cmake, os.X_OK)
    return seen


def main() -> int:
    argv = sys.argv[1:]
    if "--dry-run" in argv:
        return support.dry_run()
    if support.WINDOWS:
        print("SKIP: the Windows host unpacks the zip with tar.exe; this test is for the POSIX host")
        return SKIPPED
    check = support.Checks()
    cc = support.find_compiler(argv)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stub = support.make_stub(tmp, cc)
        probe, why = support.build_probe(tmp, cc)
        if probe is None:
            print(why)
            return 1 if why.startswith("FAIL") else SKIPPED

        seen = install_with(probe, tmp, stub, "python3", "python3", sys.executable)
        check(seen.get("ret") == "1" and seen["new"] and not seen["old"],
              "python3 only on PATH (no unzip): the new pack is installed", seen)
        check(seen["new"] and seen["cmake_executable"],
              "python3 only on PATH: the new pack's cmake is executable (the zip's modes are kept)", seen)

        for name in ("unzip", "bsdtar", "cmake"):
            program = shutil.which(name, path=os.pathsep.join(SYSTEM_PATH + ("/usr/local/bin",)))
            if program is None:
                print("SKIP %s only on PATH: this host has no %s" % (name, name))
                continue
            seen = install_with(probe, tmp, stub, name, name, program)
            check(seen.get("ret") == "1" and seen["new"] and seen["cmake_executable"],
                  "%s only on PATH: the new pack is installed and its cmake is executable" % name, seen)

        seen = install_with(probe, tmp, stub, "none", "", None)
        check(seen.get("ret") == "0" and SENTENCE in seen.get("err", ""),
              "nothing on PATH: the install is refused and the sentence names the extractors", seen)
        check(seen["old"] and not seen["new"], "nothing on PATH: the installed pack is what it was", seen)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: the setup host unpacks the toolchain zip without unzip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
