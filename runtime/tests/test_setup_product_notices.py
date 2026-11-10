"""A product that setup builds ends with the licenses/ folder a build host's product has.

Found by comparing a set-up install with the private product of the same build
(PS1B-333): the product had 20 licence files the install lacked. Seventeen sat
in the package's own trees and were never copied beside the game; three
(SDL3, zlib, the llvm-mingw runtime) were in no package at all.

release_stage.stage_product_notices writes the folder at rebuild, by Workbench
Studio's name rule and layout (workbench_output.stage_payload and
stage_linked_notices), from texts the package carries. The package's own
licenses/toolchain folder is staged by tools/package_setup_host.sh.

Hermetic: small made-up trees; no compiler, no download.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_notices_under_test", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)

import release_stage as rs  # noqa: E402


def put(path, text="text"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def files_under(folder):
    folder = Path(folder)
    return sorted(p.relative_to(folder).as_posix() for p in folder.rglob("*") if p.is_file())


def make_package(root, toolchain_texts=True):
    """An unpacked single-title setup package: the project at the root, the two
    shared trees, and the package's own licenses/toolchain folder."""
    put(root / "game.toml", '[game]\nname = "fixture"\n')
    put(root / "LICENSE", "the kit's licence")
    put(root / "THIRD_PARTY_NOTICES.md", "the kit's notices")
    put(root / "README.md", "not a licence")
    put(root / "mods" / "preloaded" / "packages" / "x.fix" / "1.0" / "LICENSE.txt", "a mod's licence")
    fw = root / "psxrecomp"
    put(fw / "LICENSE", "framework")
    put(fw / "THIRD_PARTY_ATTRIBUTION.md", "attribution")
    put(fw / "bios" / "OpenBIOS.toml", "profile")              # what makes this folder the framework
    put(fw / "recompiler" / "lib" / "fmt" / "LICENSE.rst", "fmt")
    put(fw / "recompiler" / "lib" / "fmt" / "doc" / "python-license.txt", "not by the name rule")
    put(fw / "recompiler" / "build" / "_deps" / "x" / "LICENSE", "a build folder: never")
    put(fw / "lib" / "recomp-net" / "LICENSE", "recomp-net")
    put(fw / "runtime" / "licenses" / "libchdr-NOTICES.txt", "libchdr")
    put(fw / "runtime" / "licenses" / "toml11-NOTICES.txt", "toml11")
    put(fw / "tools" / "licenses" / "LGPL-2.1.txt", "not by the name rule")
    ui = root / "recomp-ui"
    put(ui / "LICENSE", "ui")
    put(ui / "assets" / "common" / "fonts" / "NOTICE.md", "fonts")
    put(ui / "src" / "third_party" / "imgui" / "LICENSE.txt", "imgui")
    put(ui / "build" / "LICENSE", "a build folder: never")
    if toolchain_texts:
        put(root / "licenses" / "toolchain" / "SDL3-LICENSE.txt", "SDL3")
        put(root / "licenses" / "toolchain" / "zlib-LICENSE.txt", "zlib")
        put(root / "licenses" / "toolchain" / "llvm-mingw-LICENSE.TXT", "llvm-mingw")
    # what setup and a build leave at the root: none of it is the kit's
    put(root / "build-release" / "LICENSE", "left by an earlier run")
    put(root / "generated" / "NOTICE", "generated")
    put(root / "disc" / "LICENSE", "disc working tree")
    return fw, ui


EXPECTED = [
    "framework/LICENSE",
    "framework/THIRD_PARTY_ATTRIBUTION.md",
    "framework/lib/recomp-net/LICENSE",
    "framework/recompiler/lib/fmt/LICENSE.rst",
    "framework/runtime/licenses/libchdr-NOTICES.txt",
    "framework/runtime/licenses/toml11-NOTICES.txt",
    "kit/LICENSE",
    "kit/THIRD_PARTY_NOTICES.md",
    "kit/mods/preloaded/packages/x.fix/1.0/LICENSE.txt",
    "toolchain/SDL3-LICENSE.txt",
    "toolchain/llvm-mingw-LICENSE.TXT",
    "toolchain/zlib-LICENSE.txt",
    "ui/LICENSE",
    "ui/assets/common/fonts/NOTICE.md",
    "ui/src/third_party/imgui/LICENSE.txt",
]


class NameRule(unittest.TestCase):
    def test_which_files_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("LICENSE", "license.txt", "COPYING", "Notice.md", "THIRD_PARTY_NOTICES.md",
                         "sub/LICENSE.rst", "sub/deeper/copying.lesser"):
                put(root / name)
            for name in ("README.md", "python-license.txt", "libchdr-NOTICES.txt", "OpenBIOS.LICENSE",
                         ".git/LICENSE", "build/LICENSE", "sub/build-release/NOTICE", "skipped/LICENSE"):
                put(root / name)
            self.assertEqual(rs.notice_files(str(root), ("skipped",)),
                             ["COPYING", "LICENSE", "Notice.md", "THIRD_PARTY_NOTICES.md", "license.txt",
                              "sub/LICENSE.rst", "sub/deeper/copying.lesser"])
            # a skipped name applies at the top only
            put(root / "sub" / "skipped" / "LICENSE")
            self.assertIn("sub/skipped/LICENSE", rs.notice_files(str(root), ("skipped",)))


class ProductNotices(unittest.TestCase):
    def test_the_folder_a_build_hosts_product_has(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "package"
            fw, ui = make_package(root)
            stage = root / "build-release"
            put(stage / "licenses" / "toolchain" / "tcc-COPYING.txt", "written by stage_tcc_notices")
            lines = []
            counts = rs.stage_product_notices(str(stage), str(fw), ui=str(ui), project=str(root), log=lines.append)
            self.assertEqual(files_under(stage / "licenses"),
                             sorted(EXPECTED + ["toolchain/tcc-COPYING.txt"]))
            self.assertEqual(counts, {"kit": 3, "framework": 6, "ui": 3, "toolchain": 3})
            self.assertEqual((stage / "licenses" / "toolchain" / "SDL3-LICENSE.txt").read_text(encoding="utf-8"),
                             "SDL3")
            self.assertEqual((stage / "licenses" / "toolchain" / "tcc-COPYING.txt").read_text(encoding="utf-8"),
                             "written by stage_tcc_notices")
            self.assertTrue(any("licence texts staged" in line for line in lines))
            self.assertFalse(any("carries no licence texts" in line for line in lines))
            # the file left in build-release by an earlier run is not a kit file
            self.assertFalse((stage / "licenses" / "kit" / "build-release").exists())

    def test_a_program_of_a_set_takes_the_toolchain_texts_from_the_sets_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            fw, ui = make_package(root)                     # the set's root carries licenses/toolchain
            program = root / "programs" / "leon"
            put(program / "game.toml", "[game]\n")
            put(program / "LICENSE", "leon's kit licence")
            stage = program / "build-release"
            counts = rs.stage_product_notices(str(stage), str(fw), ui=str(ui), project=str(program),
                                              package=str(root), log=lambda line: None)
            self.assertEqual(counts["toolchain"], 3)
            self.assertEqual(counts["kit"], 1)
            self.assertEqual((stage / "licenses" / "kit" / "LICENSE").read_text(encoding="utf-8"),
                             "leon's kit licence")
            self.assertTrue((stage / "licenses" / "toolchain" / "zlib-LICENSE.txt").is_file())

    def test_a_package_without_toolchain_texts_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "package"
            fw, ui = make_package(root, toolchain_texts=False)
            lines = []
            counts = rs.stage_product_notices(str(root / "build-release"), str(fw), ui=str(ui),
                                              project=str(root), log=lines.append)
            self.assertEqual(counts["toolchain"], 0)
            self.assertTrue(any("carries no licence texts for its toolchain libraries" in line for line in lines))
            self.assertFalse((root / "build-release" / "licenses" / "toolchain").exists())

    def test_it_never_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "package"
            fw, ui = make_package(root)
            lines = []
            with patch.object(rs.shutil, "copyfile", side_effect=OSError("disk full")):
                counts = rs.stage_product_notices(str(root / "build-release"), str(fw), ui=str(ui),
                                                  project=str(root), log=lines.append)
            self.assertEqual(counts["toolchain"], 0)
            self.assertTrue(any("WARNING" in line and "disk full" in line for line in lines))
            # nothing given at all: nothing written, no error
            self.assertEqual(rs.stage_product_notices(str(root / "other"), "", log=lines.append)["framework"], 0)


def tree_bytes(folder):
    folder = Path(folder)
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in sorted(folder.rglob("*")) if p.is_file()}


class OwnOutput(unittest.TestCase):
    """The step never reads what it writes, wherever the product folder is.

    Found on the final Windows run of the candidate (PS1B-333): one test had the
    product folder at the framework's own root. The framework scan then read the
    kit folder the same run had just written, and the next run read the whole
    licenses/ folder of the one before: 39 files, three folders deep, in the
    source tree. Each case here runs the step more than once and compares.
    """

    def test_exclude_leaves_a_folder_out_of_the_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "tree"
            for name in ("LICENSE", "sub/NOTICE", "sub/deeper/COPYING", "other/LICENSE"):
                put(root / name)
            put(Path(tmp) / "outside" / "LICENSE")
            everything = ["LICENSE", "other/LICENSE", "sub/NOTICE", "sub/deeper/COPYING"]
            self.assertEqual(sorted(rs.notice_files(str(root))), everything)
            self.assertEqual(sorted(rs.notice_files(str(root), exclude=[str(root / "sub")])),
                             ["LICENSE", "other/LICENSE"])
            self.assertEqual(sorted(rs.notice_files(str(root), exclude=[str(root / "sub" / "deeper")])),
                             ["LICENSE", "other/LICENSE", "sub/NOTICE"])
            # a folder that is not there yet, the root itself, a folder outside, an empty name
            self.assertEqual(sorted(rs.notice_files(str(root), exclude=[str(root / "licenses"), str(root),
                                                                        str(Path(tmp) / "outside"), ""])),
                             everything)

    def test_a_second_run_writes_the_same_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "package"
            fw, ui = make_package(root)
            stage = root / "build-release"
            first = rs.stage_product_notices(str(stage), str(fw), ui=str(ui), project=str(root),
                                             log=lambda line: None)
            after_first = tree_bytes(root)
            second = rs.stage_product_notices(str(stage), str(fw), ui=str(ui), project=str(root),
                                              log=lambda line: None)
            self.assertEqual(second, first)
            self.assertEqual(tree_bytes(root), after_first)
            self.assertEqual(files_under(stage / "licenses"), sorted(EXPECTED))

    def test_a_product_folder_at_the_root_of_the_framework(self):
        # The shape of recompiler/tests/test_pgo_rebuild_flow.py before it replaced the step:
        # the framework checkout is the project, and the executable sits at its root.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "checkout"
            put(root / "LICENSE", "framework")
            put(root / "bios" / "OpenBIOS.toml", "profile")
            put(root / "recompiler" / "lib" / "fmt" / "LICENSE.rst", "fmt")
            put(root / "runtime" / "licenses" / "libchdr-NOTICES.txt", "libchdr")
            expected = ["framework/LICENSE", "framework/recompiler/lib/fmt/LICENSE.rst",
                        "framework/runtime/licenses/libchdr-NOTICES.txt",
                        "kit/LICENSE", "kit/recompiler/lib/fmt/LICENSE.rst"]
            seen = []
            for _ in range(3):
                lines = []
                counts = rs.stage_product_notices(str(root), str(root), project=str(root), log=lines.append)
                self.assertEqual(counts, {"kit": 2, "framework": 3, "ui": 0, "toolchain": 0})
                self.assertEqual(files_under(root / "licenses"), expected)
                self.assertFalse(any("WARNING" in line for line in lines), lines)
                seen.append(tree_bytes(root))
            self.assertEqual(seen[1], seen[0])
            self.assertEqual(seen[2], seen[0])

    def test_a_product_folder_inside_a_scanned_tree_under_any_name(self):
        # --build-dir is the caller's to name. What a build leaves in the product
        # folder (a bundled program's licence, an earlier licenses/ folder) is never
        # the kit's or the framework's own text.
        for where in ("project", "framework"):
            with self.subTest(where=where), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / "package"
                fw, ui = make_package(root)
                stage = (root if where == "project" else fw) / "my-product"
                put(stage / "overlay_toolchain" / "python" / "LICENSE.txt", "a bundled program's")
                put(stage / "_deps" / "x" / "LICENSE", "what a build fetched")
                seen = []
                for _ in range(2):
                    counts = rs.stage_product_notices(str(stage), str(fw), ui=str(ui), project=str(root),
                                                      log=lambda line: None)
                    self.assertEqual(files_under(stage / "licenses"), sorted(EXPECTED))
                    self.assertEqual(counts, {"kit": 3, "framework": 6, "ui": 3, "toolchain": 3})
                    seen.append(tree_bytes(root))
                self.assertEqual(seen[1], seen[0])

    def test_a_product_folder_that_is_the_package_root(self):
        # The package's own licenses/toolchain folder is then where the texts go:
        # nothing to copy, and no file copied onto itself.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "package"
            fw, ui = make_package(root)
            for _ in range(2):
                lines = []
                counts = rs.stage_product_notices(str(root), str(fw), ui=str(ui), project=str(root),
                                                  log=lines.append)
                self.assertEqual(counts, {"kit": 3, "framework": 6, "ui": 3, "toolchain": 3})
                self.assertEqual(files_under(root / "licenses"), sorted(EXPECTED))
                self.assertFalse(any("WARNING" in line for line in lines), lines)
                self.assertEqual((root / "licenses" / "toolchain" / "SDL3-LICENSE.txt").read_text(encoding="utf-8"),
                                 "SDL3")


class Rebuild(unittest.TestCase):
    def test_the_cli_stages_them_from_the_project(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "package"
            make_package(root)
            put(root / "psxrecomp" / "recompiler" / "CMakeLists.txt", "x")
            progress = Mock()
            counts = cli.stage_notices_for_product(root, root / "build-release", progress)
            self.assertEqual(counts, {"kit": 3, "framework": 6, "ui": 3, "toolchain": 3})
            self.assertEqual(files_under(root / "build-release" / "licenses"), sorted(EXPECTED))

    def test_rebuild_calls_it_before_the_prune_and_names_the_package_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            put(root / "game.toml", '[game]\nname = "fixture"\n')
            calls = []

            def fake_configure(project_root, build_dir, *, pgo, extra, progress):
                Path(build_dir).mkdir(parents=True, exist_ok=True)
                lines = [e[2:].replace("=", ":BOOL=", 1) for e in extra if e.startswith("-D") and "=" in e]
                (Path(build_dir) / "CMakeCache.txt").write_text("\n".join(lines), encoding="utf-8")

            def fake_notices(project_root, exe_dir, progress, package_root=""):
                calls.append(("notices", Path(exe_dir).name, package_root))
                return {}

            args = argparse.Namespace(
                config=str(root / "game.toml"), project_root=str(root), build_dir="build-release",
                target="psx-runtime", exe_basename="Fixture", disc="", no_pgo=True, force_pgo=False,
                cmake_extra=[], diagnostic_dir="", prune_after="build-intermediates",
                package_root=str(root / "the-set"))
            with patch.object(cli, "activate_embedded_toolchain", lambda *a, **k: True), \
                 patch.object(cli, "stage_overlay_toolchain_for_product",
                              side_effect=lambda *a, **k: calls.append(("toolchain",))), \
                 patch.object(cli, "stage_notices_for_product", side_effect=fake_notices), \
                 patch.object(cli, "prune_after_rebuild",
                              side_effect=lambda *a, **k: calls.append(("prune",))), \
                 patch.object(cli, "_cmake_configure", side_effect=fake_configure), \
                 patch.object(cli, "_cmake_build", lambda *a, **k: None), \
                 patch.object(cli, "_resolve_runtime_exe",
                              side_effect=lambda d, t, b: (Path(d) / "Fixture.exe", None)):
                code = cli.cmd_rebuild(args, Mock())
            self.assertEqual(code, cli.EXIT_OK)
            self.assertEqual(calls, [("toolchain",), ("notices", "build-release", str(root / "the-set")),
                                     ("prune",)])

    def test_the_prune_leaves_the_licence_folder(self):
        self.assertIn("licenses", cli.PRUNE_KEEPS)


if __name__ == "__main__":
    unittest.main()
