"""Pruning build intermediates must leave the staged product whole.

`rebuild --prune-after build-intermediates` (the setup wizard's rebuild) stages
the overlay toolchain beside the binary and then deletes every .o, .a and .lib
under the build folder. tcc's own runtime library is `tcc/lib/libtcc1-64.a`,
and `tcc -shared` needs it to link an overlay. The sweep deleted it, so the tcc
tier of a set-up install could not link. Found when a set-up install was
compared with the private product of the same build: the two libtcc1 files
were the difference (PS1B-333).
"""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_prune_under_test", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)


class Progress:
    def __init__(self):
        self.lines = []

    def log(self, message, **_):
        self.lines.append(message)


def put(path, text="x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class PruneKeepsProduct(unittest.TestCase):
    def test_build_intermediates_go_and_the_staged_product_stays(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build-release"
            kept = ["Game.exe", "game.toml", "SDL3.dll",
                    "overlay_toolchain/tcc/lib/libtcc1-64.a", "overlay_toolchain/tcc/lib/libtcc1-32.a",
                    "overlay_toolchain/tcc/lib/kernel32.def", "overlay_toolchain/tcc/tcc.exe",
                    "overlay_toolchain/python/libs/python3.lib", "overlay_toolchain/include/overlay_api.h",
                    "assets/fonts/a.ttf", "mods/bundled/x/1.0/plugin.lib", "licenses/toolchain/tcc-COPYING.txt",
                    "cache/SLUS-00001/gcc/shard.o", "saves/card1.mcd"]
            gone = ["CMakeCache.txt", "build.ninja", "CMakeFiles/psx-runtime.dir/main.cpp.obj",
                    "_deps/psx_libchdr-build/libchdr-static.a", "libruntime.a", "psx-runtime.pdb",
                    "lib/import.lib", "obj/x.o"]
            for relative in kept + gone:
                put(build / relative)
            progress = Progress()
            cli.prune_after_rebuild(Path(tmp), build, {"build-intermediates"}, progress)
            for relative in kept:
                self.assertTrue((build / relative).is_file(), relative)
            for relative in gone:
                self.assertFalse((build / relative).exists(), relative)
            self.assertTrue(any("Pruned build intermediates" in line for line in progress.lines))

    def test_the_kept_folders_are_the_product_folders(self):
        # The same names the set join takes as a program's payload folders
        # (tools/program_set.py), plus the player's saves.
        import program_set
        self.assertEqual(set(cli.PRUNE_KEEPS) - {"saves"}, set(program_set.PAYLOAD_DIRS))


if __name__ == "__main__":
    unittest.main()
