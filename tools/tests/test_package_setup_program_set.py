#!/usr/bin/env python3
"""package_setup_host.sh --set: a set of programs as one setup package (PS1B-333).

The packager needs a built host to run to the end, so this test drives it only
as far as its own decisions about the set: which set file it accepts, which
source trees it refuses, and that a valid set gets past them. The staging rules
that follow are checked in the script's text. A single-program package must
not change: the lines it depends on are asserted too.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_package_setup_bios_hint import find_bash, shell_env, to_shell_path  # noqa: E402
from test_program_set import SET_TOML, make_set  # noqa: E402


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


def main() -> int:
    check_text()
    bash = find_bash()
    if bash is None:
        print("package setup program set test: text checks PASS; SKIP the runs (no bash on this machine)")
        return 0
    syntax = subprocess.run([bash, "-n", to_shell_path(PACKAGER, bash)], capture_output=True, text=True,
                            env=shell_env(bash))
    assert syntax.returncode == 0, syntax.stderr
    with tempfile.TemporaryDirectory() as tmp:
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
    print("package setup program set test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
