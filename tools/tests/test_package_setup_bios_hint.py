#!/usr/bin/env python3
"""The packager's README BIOS wording must survive a recipe without bios_config.

package_setup_host.sh derives the player-facing BIOS hint from the staged
game.toml. The Wave 4 pilot recipe (openbios = false, no bios_config: the host
default stem applies) made the derivation's grep exit 1, and under
``set -euo pipefail`` the packager stopped silently right after
"stage_setup_sdk: ready" with no README-SETUP.txt. The derivation lives in
recipe_bios_hint(); this test runs it under the packager's shell options on
the three recipe shapes the kits use.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"

CASES = [
    # (recipe text, expected substring, forbidden substring)
    ('[recompiler]\nseeds = "x"\n\n[runtime]\nopenbios = false\n',
     "legally dumped retail PlayStation BIOS image (required", "optional"),
    ('[recompiler]\nbios_config = "psxrecomp/bios/SCPH5552.toml"\n\n[runtime]\nopenbios = false\n',
     "legally dumped SCPH5552 BIOS image (required", "optional"),
    ('[recompiler]\nbios_config = "psxrecomp/bios/SCPH1001.toml"\n\n[runtime]\nwindow_title = "t"\n',
     "an optional retail SCPH1001 BIOS dump", "required"),
    ('[recompiler]\nseeds = "x"\n',
     "an optional retail SCPH-1001 BIOS dump", "required"),
]


def find_bash() -> str | None:
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


def to_shell_path(path: Path, bash: str) -> str:
    if os.name != "nt":
        return str(path)
    out = subprocess.run([bash, "-c", 'cygpath -u "$1"', "_", str(path)],
                         capture_output=True, text=True)
    return out.stdout.strip() or str(path)


def main() -> int:
    bash = find_bash()
    if bash is None:
        print("package setup bios hint test: SKIP (no bash on this machine)")
        return 0
    text = PACKAGER.read_text(encoding="utf-8")
    assert "recipe_bios_hint() {" in text
    assert 'BIOS_HINT="$(recipe_bios_hint "${STAGE}/game.toml")"' in text
    start = text.index("recipe_bios_hint() {")
    end = text.index("\n}\n", start) + 3
    func = text[start:end]
    with tempfile.TemporaryDirectory() as tmp:
        for recipe, expect, forbid in CASES:
            toml = Path(tmp) / "game.toml"
            toml.write_text(recipe, encoding="utf-8", newline="\n")
            script = "set -euo pipefail\n" + func + '\nrecipe_bios_hint "$1"\necho "rc=$?"\n'
            run = subprocess.run([bash, "-c", script, "_", to_shell_path(toml, bash)],
                                 capture_output=True, text=True)
            out = run.stdout.replace("\r", "")
            assert run.returncode == 0, (run.returncode, out, run.stderr)
            assert out.endswith("rc=0\n"), out
            hint = out[: -len("rc=0\n")].strip()
            assert expect in hint, (recipe, hint)
            assert forbid not in hint, (recipe, hint)
    print("package setup bios hint test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
