#!/usr/bin/env python3
"""Tests for duckstation_oracle.py's decision logic.

Nothing here builds or downloads anything. What is pinned is the reasoning that
is easy to get wrong and expensive to discover late: where the install lands,
which hosts upstream refuses, that the dependency bundle is verified against the
checkout's own checksum file rather than a hardcoded one, and that the settings
merge preserves DuckStation's own defaults.
"""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

_spec = importlib.util.spec_from_file_location("dso", ROOT / "duckstation_oracle.py")
DSO = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
_spec.loader.exec_module(DSO)


class EnvGuard(unittest.TestCase):
    def setUp(self):
        self._env = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)


class TestLocations(EnvGuard):
    def test_install_is_outside_any_game_repo(self):
        os.environ.pop("RETCOMM_ORACLE_DIR", None)
        os.environ.pop("RETCOMM_DATA_DIR", None)
        root = DSO.oracle_root()
        self.assertEqual(root.parts[-2:], ("oracle", "duckstation"))
        # The whole point: one build shared by every title, not per-repo.
        self.assertIn("retcomm", str(root))
        self.assertNotIn("psxrecomp", str(root))

    def test_data_dir_override_moves_everything(self):
        os.environ.pop("RETCOMM_ORACLE_DIR", None)
        os.environ["RETCOMM_DATA_DIR"] = "/tmp/rc-test"
        self.assertEqual(DSO.oracle_root(), Path("/tmp/rc-test/oracle/duckstation"))
        self.assertEqual(DSO.download_cache(), Path("/tmp/rc-test/downloads"))

    def test_xdg_data_home_is_honoured_like_studio_does(self):
        """studio_runner.cpp's retcomm_data_dir() checks XDG_DATA_HOME before
        ~/.local/share. If this tool did not, the GUI would look for an oracle
        somewhere other than where the tool installed it, and neither side would
        look wrong."""
        os.environ.pop("RETCOMM_ORACLE_DIR", None)
        os.environ.pop("RETCOMM_DATA_DIR", None)
        os.environ["XDG_DATA_HOME"] = "/tmp/xdg-test"
        self.assertEqual(DSO.data_root(), Path("/tmp/xdg-test/retcomm"))
        self.assertEqual(DSO.oracle_root(),
                         Path("/tmp/xdg-test/retcomm/oracle/duckstation"))

    def test_retcomm_data_dir_beats_xdg(self):
        os.environ.pop("RETCOMM_ORACLE_DIR", None)
        os.environ["XDG_DATA_HOME"] = "/tmp/xdg-test"
        os.environ["RETCOMM_DATA_DIR"] = "/tmp/explicit"
        self.assertEqual(DSO.data_root(), Path("/tmp/explicit"))

    def test_oracle_dir_override_wins(self):
        os.environ["RETCOMM_DATA_DIR"] = "/tmp/rc-test"
        os.environ["RETCOMM_ORACLE_DIR"] = "/tmp/elsewhere"
        self.assertEqual(DSO.oracle_root(), Path("/tmp/elsewhere"))

    def test_layout_paths_hang_off_the_root(self):
        lay = DSO.Layout(Path("/tmp/o"))
        self.assertEqual(lay.src, Path("/tmp/o/src"))
        self.assertEqual(lay.build, Path("/tmp/o/build"))
        self.assertEqual(lay.app, Path("/tmp/o/app"))
        self.assertEqual(lay.built_binary, Path("/tmp/o/build/bin") / DSO.exe_name())
        self.assertEqual(lay.binary, Path("/tmp/o/app") / DSO.exe_name())


class TestRefusedEnvironment(EnvGuard):
    """Upstream's CMake refuses Arch-family and NixOS hosts. We predict that
    rather than letting it surface as 'Unsupported environment.' 20 seconds into
    a configure, and we never patch the check out."""

    def _osr(self, text):
        tmp = tempfile.NamedTemporaryFile("w", suffix=".osr", delete=False)
        tmp.write(text)
        tmp.close()
        self.addCleanup(os.unlink, tmp.name)
        return Path(tmp.name)

    def test_detects_each_refused_marker(self):
        # Literal substrings, matching what upstream's CMake matches
        # (MATCHES "ID_LIKE=arch"). Mirroring it exactly matters: if upstream
        # would NOT refuse a host, we must not route that host to a container.
        for marker in ("ID=arch\n", "ID=cachyos\nID_LIKE=arch\n", "ID=nixos\n"):
            with self.subTest(marker=marker):
                osr = self._osr(marker)
                orig = DSO.Path
                try:
                    DSO.Path = lambda p, _o=orig, _f=osr: _f if p == "/etc/os-release" else _o(p)
                    self.assertIsNotNone(DSO.refused_environment())
                finally:
                    DSO.Path = orig

    def test_plain_distro_is_not_refused(self):
        osr = self._osr('ID=ubuntu\nID_LIKE="debian"\n')
        orig = DSO.Path
        try:
            DSO.Path = lambda p, _o=orig, _f=osr: _f if p == "/etc/os-release" else _o(p)
            for var in ("NIX_BUILD_TOP", "NIX_STORE", "IN_NIX_SHELL"):
                os.environ.pop(var, None)
            self.assertIsNone(DSO.refused_environment())
        finally:
            DSO.Path = orig

    def test_nix_env_vars_are_refused_too(self):
        osr = self._osr("ID=ubuntu\n")
        orig = DSO.Path
        try:
            DSO.Path = lambda p, _o=orig, _f=osr: _f if p == "/etc/os-release" else _o(p)
            os.environ["IN_NIX_SHELL"] = "1"
            self.assertIn("IN_NIX_SHELL", DSO.refused_environment() or "")
        finally:
            DSO.Path = orig


class TestDepsVerification(unittest.TestCase):
    def test_checksum_comes_from_the_checkout_not_a_constant(self):
        """A hardcoded hash pairs a new source tree with an old bundle the first
        time the pin moves. Read it from the tree that is actually checked out."""
        with tempfile.TemporaryDirectory() as td:
            src = Path(td)
            (src / "dep").mkdir()
            (src / "dep" / "PREBUILT-SHA256SUMS").write_text(
                "aaaa *deps-linux-x64.tar.xz\n"
                "bbbb *deps-windows-x64.7z\n")
            self.assertEqual(DSO.expected_sha(src, "deps-linux-x64.tar.xz"), "aaaa")
            self.assertEqual(DSO.expected_sha(src, "deps-windows-x64.7z"), "bbbb")
            self.assertIsNone(DSO.expected_sha(src, "deps-macos-universal.tar.xz"))

    def test_missing_sums_file_is_not_fatal(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertIsNone(DSO.expected_sha(Path(td), "deps-linux-x64.tar.xz"))

    def test_platform_bundle_selection(self):
        archive, dirname = DSO.deps_platform()
        self.assertTrue(archive.startswith("deps-"))
        self.assertIn(dirname, ("linux-x64", "windows-x64", "macos-universal"))


class TestSettingsMerge(unittest.TestCase):
    def test_merge_preserves_duckstations_own_defaults(self):
        """We override a handful of fields; the emulator owns the rest of the
        schema. Rewriting the whole file would mean guessing at it."""
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "settings.ini"
            p.write_text("[Main]\nEmulationSpeed = 1\nSaveStateOnExit = true\n\n"
                         "[Display]\nVSync = false\n")
            DSO.merge_ini(p, {"Main": {"ConfirmPowerOff": "false"},
                              "UI": {"UnofficialBuildWarningConfirmed": "true"}})
            out = p.read_text()
            self.assertIn("EmulationSpeed = 1", out)      # untouched
            self.assertIn("VSync = false", out)           # untouched section
            self.assertIn("ConfirmPowerOff = false", out)  # added
            self.assertIn("UnofficialBuildWarningConfirmed = true", out)

    def test_merge_overwrites_a_key_it_owns(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "settings.ini"
            p.write_text("[Main]\nConfirmPowerOff = true\n")
            DSO.merge_ini(p, {"Main": {"ConfirmPowerOff": "false"}})
            self.assertIn("ConfirmPowerOff = false", p.read_text())

    def test_keys_stay_case_sensitive(self):
        # DuckStation reads CamelCase keys; configparser lowercases by default,
        # which would silently produce a file it ignores.
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "settings.ini"
            DSO.merge_ini(p, {"BIOS": {"PathNTSC-U": "SCPH1001.BIN"}})
            self.assertIn("PathNTSC-U", p.read_text())

    def test_merge_creates_the_file_when_absent(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "settings.ini"
            DSO.merge_ini(p, {"GPU": {"Renderer": "Software"}})
            self.assertIn("Renderer = Software", p.read_text())


class TestStatusDoc(EnvGuard):
    """The status document is the whole contract with Retro Studio."""

    def test_absent_install_reports_absent(self):
        os.environ["RETCOMM_ORACLE_DIR"] = tempfile.mkdtemp()
        doc = DSO.status_doc(DSO.Layout())
        self.assertEqual(doc["kind"], "psxrecomp-oracle-status")
        self.assertEqual(doc["state"], "absent")
        for k in ("source", "deps", "built", "installed", "running", "answering"):
            self.assertFalse(doc[k], k)
        self.assertIn("port", doc)
        self.assertIn("container_needed", doc)

    def test_state_ladder_is_ordered(self):
        """A GUI picks its button off `state`, so the ladder must not skip."""
        os.environ["RETCOMM_ORACLE_DIR"] = tempfile.mkdtemp()
        lay = DSO.Layout()
        lay.src.mkdir(parents=True)
        (lay.src / "CMakeLists.txt").write_text("")
        self.assertEqual(DSO.status_doc(lay)["state"], "fetched")
        (lay.build / "bin").mkdir(parents=True)
        (lay.build / "bin" / DSO.exe_name()).write_text("")
        self.assertEqual(DSO.status_doc(lay)["state"], "built")
        lay.app.mkdir(parents=True)
        (lay.app / DSO.exe_name()).write_text("")
        self.assertEqual(DSO.status_doc(lay)["state"], "installed")

    def test_kind_survives_an_existing_manifest(self):
        """oracle.json has its own `kind`. Merging it must not overwrite the
        status document's, or every INSTALLED oracle reports a document Studio
        rejects as foreign — while an uninstalled one parses fine, so the bug
        only appears once things are working."""
        os.environ["RETCOMM_ORACLE_DIR"] = tempfile.mkdtemp()
        lay = DSO.Layout()
        lay.root.mkdir(parents=True, exist_ok=True)
        lay.manifest.write_text(json.dumps({
            "kind": "psxrecomp-duckstation-oracle",
            "version": 1,
            "upstream_base": "deadbeef",
        }))
        doc = DSO.status_doc(lay)
        self.assertEqual(doc["kind"], "psxrecomp-oracle-status")
        self.assertEqual(doc["upstream_base"], "deadbeef")  # manifest data kept

    def test_json_is_serialisable(self):
        os.environ["RETCOMM_ORACLE_DIR"] = tempfile.mkdtemp()
        json.dumps(DSO.status_doc(DSO.Layout()))


class TestPin(unittest.TestCase):
    def test_pin_is_the_single_source_of_truth(self):
        pin = DSO.load_pin()
        self.assertRegex(pin["upstream_base"], r"^[0-9a-f]{40}$")
        self.assertTrue(pin["upstream_url"].endswith("duckstation.git"))
        self.assertEqual(pin["oracle_port"], 4371)
        self.assertTrue((DSO.PATCH_DIR / pin["patch"]).is_file())

    def test_windows_setup_reads_the_same_pin(self):
        # The two platform paths must never drift onto different bases.
        setup = (DSO.PATCH_DIR / "setup.sh").read_text()
        self.assertIn("pin.json", setup)
        self.assertNotIn('UPSTREAM_BASE="ffb33c', setup)

    def test_image_tag_tracks_the_pin(self):
        pin = DSO.load_pin()
        self.assertTrue(DSO.image_tag(pin).endswith(pin["upstream_base"][:12]))


class TestOraclePatch(unittest.TestCase):
    """The patch is stored with added lines only (PS1B-216). A context line or a
    removed line is a line of upstream's source, and this repository must not
    hold one."""

    OP = DSO.oracle_patch_module()

    def git(self, root, *args):
        import subprocess
        return subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t",
                               "-c", "core.autocrlf=false", *args],
                              capture_output=True, text=True, check=True).stdout

    def test_stored_patch_holds_no_upstream_line(self):
        pin = DSO.load_pin()
        text = (DSO.PATCH_DIR / pin["patch"]).read_text(encoding="utf-8")
        self.assertEqual(self.OP.problems(text), [])
        hunks = [line for line in text.split("\n") if line.startswith("@@")]
        self.assertGreater(len(hunks), 0)
        for line in hunks:
            self.assertRegex(line, r"^@@ -\d+,0 \+\d+,\d+ @@$")
        body = [line for line in text.split("\n")
                if line and not line.startswith(("diff --git ", "index ", "new file mode ", "--- ", "+++ ", "@@", "+"))]
        self.assertEqual(body, [])

    def test_a_context_patch_is_reported(self):
        diff = ("diff --git a/f.txt b/f.txt\nindex 1111111..2222222 100644\n--- a/f.txt\n+++ b/f.txt\n"
                "@@ -2,3 +2,4 @@ some function\n two\n+new\n three\n four\n")
        self.assertEqual(len(self.OP.problems(diff)), 5)   # header text, old lines, 3 context lines

    def test_zero_context_keeps_added_lines_and_reports_rewrites(self):
        diff = ("diff --git a/f.txt b/f.txt\nindex 1111111..2222222 100644\n--- a/f.txt\n+++ b/f.txt\n"
                "@@ -2,5 +2,6 @@ some function\n two\n+new\n three\n-four\n+FOUR\n five\n six\n"
                "diff --git a/g.txt b/g.txt\nnew file mode 100644\nindex 0000000..3333333\n--- /dev/null\n+++ b/g.txt\n"
                "@@ -0,0 +1,2 @@\n+a\n+b\n")
        text, removed = self.OP.zero_context(diff)
        self.assertEqual(removed, [{"path": "f.txt", "line": 4, "replaced": True}])
        self.assertEqual(self.OP.problems(text), [])
        self.assertIn("@@ -2,0 +3,1 @@\n+new\n", text)
        self.assertIn("@@ -0,0 +1,2 @@\n+a\n+b\n", text)
        self.assertNotIn("FOUR", text)
        self.assertNotIn("some function", text)

    def test_apply_on_a_stand_in_checkout(self):
        """Line edit, base check, apply, repeat, and the refusal of a wrong base."""
        import shutil
        if shutil.which("git") is None:
            self.skipTest("git is not on PATH; the stored-patch form is still checked above")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "checkout"
            root.mkdir()
            self.git(root, "init", "-q")
            lines = [f"line {i}" for i in range(1, 11)]
            lines[5] = "call(A SOURCES B)"
            (root / "f.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
            (root / "build.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
            self.git(root, "add", "-A")
            self.git(root, "commit", "-q", "-m", "base")
            blob = self.git(root, "rev-parse", "HEAD:f.txt").strip()
            patch = Path(td) / "p.patch"
            patch.write_text("diff --git a/f.txt b/f.txt\nindex %s..%s 100644\n--- a/f.txt\n+++ b/f.txt\n"
                             "@@ -3,0 +4,2 @@\n+ours 1\n+ours 2\n" % (blob[:7], "f" * 7),
                             encoding="utf-8", newline="\n")
            pin = {"patch": str(patch), "line_edits": [{
                "path": "build.txt", "line": 6, "remove_text": "SOURCES ", "upstream_blob": blob[:7],
                "sha256_before": self.OP.line_hash("call(A SOURCES B)"),
                "sha256_after": self.OP.line_hash("call(A B)")}]}
            self.assertFalse(self.OP.is_applied(root, pin))
            self.assertEqual(self.OP.apply(root, pin), "applied")
            self.assertEqual((root / "f.txt").read_text(encoding="utf-8").split("\n")[2:6],
                             ["line 3", "ours 1", "ours 2", "line 4"])
            self.assertEqual((root / "build.txt").read_text(encoding="utf-8").split("\n")[5], "call(A B)")
            self.assertTrue(self.OP.is_applied(root, pin))
            self.assertEqual(self.OP.apply(root, pin), "already applied")
            # A changed file that is not the patched one is refused, not patched twice.
            self.git(root, "checkout", "--", "f.txt")
            (root / "f.txt").write_text("other\n" + (root / "f.txt").read_text(encoding="utf-8"),
                                        encoding="utf-8", newline="\n")
            with self.assertRaises(self.OP.PatchError):
                self.OP.apply(root, pin)
            # A base whose blob is another one is refused.
            wrong = dict(pin, patch=str(Path(td) / "wrong.patch"))
            Path(wrong["patch"]).write_text(patch.read_text(encoding="utf-8").replace(blob[:7], "0" * 7),
                                            encoding="utf-8", newline="\n")
            self.git(root, "checkout", "--", "f.txt")
            with self.assertRaises(self.OP.PatchError):
                self.OP.apply(root, wrong)

    def test_line_edits_carry_no_upstream_line(self):
        for edit in DSO.load_pin().get("line_edits", []):
            self.assertEqual(sorted(edit), ["line", "path", "remove_text", "sha256_after", "sha256_before",
                                            "upstream_blob"])
            self.assertRegex(edit["sha256_before"], r"^[0-9a-f]{64}$")
            self.assertLessEqual(len(edit["remove_text"]), 16)

    def test_both_setup_paths_use_the_helper(self):
        self.assertIn("oracle_patch.py", (DSO.PATCH_DIR / "setup.sh").read_text(encoding="utf-8"))
        self.assertNotIn("git apply", (DSO.PATCH_DIR / "setup.sh").read_text(encoding="utf-8"))


class TestBiosDiscovery(EnvGuard):
    def test_explicit_path_wins_and_missing_is_none(self):
        with tempfile.TemporaryDirectory() as td:
            bios = Path(td) / "SCPH1001.BIN"
            bios.write_bytes(b"\x00" * 16)
            self.assertEqual(DSO.find_bios(str(bios)), bios)
            self.assertIsNone(DSO.find_bios(str(Path(td) / "gone.bin")))

    def test_never_invents_one(self):
        os.environ["RETCOMM_DATA_DIR"] = tempfile.mkdtemp()
        # No index, no repo bios beside this checkout -> None, never a download.
        self.assertIn(DSO.find_bios(None), (None, DSO.find_bios(None)))


if __name__ == "__main__":
    unittest.main()
