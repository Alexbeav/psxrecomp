#!/usr/bin/env python3
"""Linux release packages ship a .tar.gz beside the zip, exec bits kept.

Runs the real archive tail of tools/package_game_release.sh on a made-up
stage, the CI template's tarball gate on its output, and the setup-package
audit on a tarball. Synthetic files only; needs bash (Git Bash on Windows).
"""
import importlib.util
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import host_bash  # noqa: E402

PACKAGER = ROOT / "tools" / "package_game_release.sh"
TEMPLATE = ROOT / "docs" / "ci" / "templates" / "game-release.yml"
spec = importlib.util.spec_from_file_location(
    "platform_copy", ROOT / "tools/ci/audit_setup_package_platform_copy.py")
platform_copy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(platform_copy)

EXE = "Fixture_Recompiled"
EXECUTABLES = {EXE, "overlay_toolchain/tcc/tcc", "overlay_toolchain/run.sh"}


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", **kwargs)


def packager_tail():
    """The archive naming and the archive step, exactly as shipped."""
    text = PACKAGER.read_text(encoding="utf-8")
    start = text.index('ZIP_NAME="${ZIP_PREFIX:?}')
    naming = text[start:text.index('rm -rf "${STAGE:?}"', start)]
    tail = text[text.rindex('if [[ "${STAGE_ONLY}" == 1 ]]; then'):]
    return naming + "\n" + tail


def template_tarball_gate():
    """The Python body of the template's linux tarball check."""
    text = TEMPLATE.read_text(encoding="utf-8")
    match = re.search(r"<<'PY'\n(.*?)\n\s*PY\n", text, re.S)
    if match is None:
        raise AssertionError("game-release.yml has no tarball gate")
    lines = match.group(1).splitlines()
    indent = min(len(l) - len(l.lstrip()) for l in lines if l.strip())
    return "\n".join(l[indent:] for l in lines) + "\n"


class ReleaseTarballTests(unittest.TestCase):
    def setUp(self):
        # On Windows a bare `bash` may be WSL's; CMake passes Git Bash here,
        # and the shared finder never returns the launcher (PS1B-450).
        self.bash = os.environ.get("PSX_PACKAGE_TEST_BASH")
        if not self.bash:
            try:
                self.bash = host_bash.find_bash("the release tarball test")
            except AssertionError as exc:
                self.skipTest(str(exc))
        self.tmp = tempfile.TemporaryDirectory(prefix="psx-release-tarball-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.stage = self.work / "dist" / "stage-game"
        files = {
            EXE: b"not-an-elf: named by --executable",
            "README.txt": f"Fixture 0.1.0\n\n1. Run {EXE}.\n2. Play.\n".encode(),
            "overlay_toolchain/tcc/tcc": b"\x7fELF fixture",
            "overlay_toolchain/run.sh": b"#!/bin/sh\nexit 0\n",
            "assets/fonts/a.ttf": b"font",
            "mods/bundled/x/manifest.toml": b'id = "x"\n',
            "game.toml": b"[runtime]\noverlay_cache = true\n",
        }
        for rel, data in files.items():
            path = self.stage / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            os.chmod(path, 0o644)

    def package(self, artifact):
        env = dict(ZIP_PREFIX="fx", VERSION="0.1.0", CHANNEL_SUFFIX="", ARTIFACT=artifact,
                   STAGE_ONLY="0", EXE_BASENAME=EXE, PY=Path(sys.executable).as_posix(),
                   SCRIPT_DIR=(ROOT / "tools").as_posix(), STAGE=self.stage.as_posix(),
                   DIST=(self.work / "dist").as_posix())
        script = "set -euo pipefail\n" + "".join(
            f"{key}='{value}'\n" for key, value in env.items()) + packager_tail()
        result = run([self.bash, "-c", script], cwd=self.work)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return self.work / "dist" / f"fx-0.1.0-{artifact}.zip"

    def test_linux_package_ships_tarball_with_exec_bits(self):
        archive = self.package("linux-x64")
        tarball = archive.with_name("fx-0.1.0-linux-x64.tar.gz")
        self.assertTrue(archive.is_file())
        self.assertTrue(tarball.is_file(), "no .tar.gz beside the linux zip")
        with zipfile.ZipFile(archive) as z:
            zip_names = sorted(z.namelist())
        with tarfile.open(tarball, "r:gz") as t:
            members = {m.name: m for m in t.getmembers()}
            self.assertEqual(t.extractfile(members[EXE]).read(),
                             (self.stage / EXE).read_bytes())
        self.assertEqual(sorted(members), zip_names)
        self.assertTrue(all(m.isfile() for m in members.values()))
        for name, member in members.items():
            want = 0o755 if name in EXECUTABLES else 0o644
            self.assertEqual(member.mode & 0o777, want, name)
            self.assertEqual((member.uid, member.gid), (0, 0), name)

        gate = template_tarball_gate()
        result = run([sys.executable, "-I", "-c", gate, str(archive), str(tarball)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("tarball verified", result.stdout)

        # The gate refuses a tarball whose game lost its exec bit.
        broken = self.work / "broken-linux-x64.tar.gz"
        with tarfile.open(tarball, "r:gz") as src, tarfile.open(broken, "w:gz") as dst:
            for member in src.getmembers():
                data = src.extractfile(member).read()
                if member.name == EXE:
                    member.mode = 0o644
                dst.addfile(member, io.BytesIO(data))
        result = run([sys.executable, "-I", "-c", gate, str(archive), str(broken)])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not mode 0755", result.stderr)

    def test_windows_package_stays_zip_only(self):
        archive = self.package("windows-x64")
        self.assertTrue(archive.is_file())
        self.assertEqual(sorted(p.name for p in archive.parent.glob("fx-*")), [archive.name])


class SetupAuditTarballTests(unittest.TestCase):
    def test_audit_reads_tarball_and_requires_0755_host(self):
        readme = ("1. Run Fixture.\nInstall CMake, Ninja, Python 3 and a C/C++ compiler; "
                  "available on PATH; native build tools\n")
        controls = ("1. Native build tools Install CMake, Ninja, Python 3, "
                    "and either Clang or GCC Check tools")
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "setup-0.1.0-linux-x64.tar.gz"

            def write(mode):
                with tarfile.open(archive, "w:gz") as out:
                    for name, text, bits in (("README-SETUP.txt", readme, 0o644),
                                             ("Fixture", controls, mode)):
                        data = text.encode()
                        info = tarfile.TarInfo(name)
                        info.size, info.mode = len(data), bits
                        out.addfile(info, io.BytesIO(data))

            write(0o755)
            self.assertEqual(platform_copy.audit(archive)["status"], "pass")
            write(0o644)
            with self.assertRaisesRegex(ValueError, "not 755"):
                platform_copy.audit(archive)


if __name__ == "__main__":
    unittest.main()
