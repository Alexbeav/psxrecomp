#!/usr/bin/env python3
"""Exercise the production SDK BIOS gate with synthetic files only.

Each case runs twice: with the TOML parser this Python has, and with tomllib and
tomli both hidden, as on the stock python3 of macOS 13, where the gate reads the
recipe with tools/program_set.py's reader (PS1B-576)."""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]


def load_gate(without_parser=False):
    """The gate module; without_parser hides tomllib and tomli while it imports."""
    hidden = ("tomllib", "tomli")
    saved = {name: sys.modules[name] for name in hidden if name in sys.modules}
    if without_parser:
        for name in hidden:
            sys.modules[name] = None   # import raises ModuleNotFoundError
    try:
        spec = importlib.util.spec_from_file_location("gate", ROOT / "tools/check_setup_bios_assets.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        for name in hidden:
            sys.modules.pop(name, None)
        sys.modules.update(saved)
    return module


gate = load_gate()
gate_without_parser = load_gate(without_parser=True)


class GateTests(unittest.TestCase):
    gate = gate

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "psxrecomp/bios").mkdir(parents=True)

    def recipe(self, text):
        (self.root / "game.toml").write_text(text)

    def asset(self, name):
        (self.root / "psxrecomp/bios" / name).write_text("synthetic fixture")

    def test_retail_profile_without_openbios(self):
        self.recipe('[runtime]\nopenbios=false\n[recompiler]\nbios_config="psxrecomp/bios/SCPH5552.toml"\n')
        self.asset("SCPH5552.toml")
        self.assertEqual(self.gate.check(self.root), [str(Path("psxrecomp/bios/SCPH5552.toml"))])

    def test_retail_missing_profile_fails(self):
        self.recipe('[runtime]\nopenbios=false\n[recompiler]\nbios_config="psxrecomp/bios/SCPH5552.toml"\n')
        with self.assertRaisesRegex(ValueError, "SCPH5552"):
            self.gate.check(self.root)

    def test_default_openbios_requires_all_assets(self):
        self.recipe('[runtime]\n')
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            with self.assertRaises(ValueError):
                self.gate.check(self.root)
            self.asset(name)
        self.assertEqual(len(self.gate.check(self.root)), 3)

    def test_external_profile_fails(self):
        self.recipe('[runtime]\nopenbios=false\n[recompiler]\nbios_config="../external.toml"\n')
        with self.assertRaisesRegex(ValueError, "inside"):
            self.gate.check(self.root)

    def test_absent_recipe_fails(self):
        with self.assertRaisesRegex(ValueError, "game.toml"):
            self.gate.check(self.root)

    def symlink(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest("symlink creation unavailable: " + str(error))

    def outside(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Path(tmp.name)

    def test_default_retail_rejects_external_profile_symlink(self):
        self.recipe('[runtime]\nopenbios=false\n')
        target = self.outside() / "SCPH1001.toml"
        target.write_text("synthetic fixture")
        self.symlink(self.root / "psxrecomp/bios/SCPH1001.toml", target)
        with self.assertRaisesRegex(ValueError, "inside"):
            self.gate.check(self.root)

    def test_openbios_rejects_external_asset_symlinks(self):
        self.recipe('[runtime]\n')
        names = ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE")
        for name in names:
            self.asset(name)
        target = self.outside() / "asset"
        target.write_text("synthetic fixture")
        for name in names:
            with self.subTest(asset=name):
                link = self.root / "psxrecomp/bios" / name
                link.unlink()
                self.symlink(link, target)
                with self.assertRaisesRegex(ValueError, "inside"):
                    self.gate.check(self.root)
                link.unlink()
                self.asset(name)

    def test_default_retail_rejects_external_bios_directory(self):
        self.recipe('[runtime]\nopenbios=false\n')
        target = self.outside()
        (target / "SCPH1001.toml").write_text("synthetic fixture")
        link = self.root / "psxrecomp/bios"
        link.rmdir()
        self.symlink(link, target, directory=True)
        with self.assertRaisesRegex(ValueError, "inside"):
            self.gate.check(self.root)

    def test_openbios_rejects_external_framework_directory(self):
        self.recipe('[runtime]\n')
        target = self.outside()
        (target / "bios").mkdir()
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            (target / "bios" / name).write_text("synthetic fixture")
        (self.root / "psxrecomp/bios").rmdir()
        link = self.root / "psxrecomp"
        link.rmdir()
        self.symlink(link, target, directory=True)
        with self.assertRaisesRegex(ValueError, "inside"):
            self.gate.check(self.root)

    def test_internal_asset_symlinks_are_accepted(self):
        self.recipe('[runtime]\n')
        target = self.root / "shared-asset"
        target.write_text("synthetic fixture")
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            self.symlink(self.root / "psxrecomp/bios" / name, target)
        self.assertEqual(len(self.gate.check(self.root)), 3)


SET_TOML = '''[set]
name = "two-programs"
title = "Two Programs"
exe_name = "Two_Programs"
serials = ["SLUS-00001", "SLUS-00002"]
programs = ["one", "two"]

[game]
discs = ["disc/disc-1.cue", "disc/disc-2.cue"]
disc_serials = ["SLUS-00001", "SLUS-00002"]

[program.one]
folder = "programs/one"
exe_name = "Program_One"
shortcut = "Two Programs - One"
serials = ["SLUS-00001"]
positions = [1]

[program.two]
folder = "programs/two"
exe_name = "Program_Two"
shortcut = "Two Programs - Two"
serials = ["SLUS-00002"]
positions = [2]
'''
RETAIL = '[runtime]\nopenbios=false\n[recompiler]\nbios_config="psxrecomp/bios/SCPH1001.toml"\n'


class SetGateTests(unittest.TestCase):
    """A set of programs has no recipe at the stage root: set.toml is there,
    and each program's game.toml is in its folder. The packager stopped on
    exactly that ("missing staged game.toml for BIOS policy", PS1B-333)."""
    gate = gate

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        (self.root / "psxrecomp/bios").mkdir(parents=True)
        (self.root / "set.toml").write_text(SET_TOML)
        for name in ("one", "two"):
            (self.root / "programs" / name).mkdir(parents=True)

    def recipe(self, program, text):
        (self.root / "programs" / program / "game.toml").write_text(text)

    def asset(self, name):
        (self.root / "psxrecomp/bios" / name).write_text("synthetic fixture")

    def test_every_program_recipe_is_checked_against_the_root(self):
        self.recipe("one", RETAIL)
        self.recipe("two", RETAIL)
        self.asset("SCPH1001.toml")
        # the profile path is the stage root's, where psxrecomp/ is; listed once
        self.assertEqual(self.gate.check(self.root), [str(Path("psxrecomp/bios/SCPH1001.toml"))])

    def test_a_missing_profile_fails(self):
        self.recipe("one", RETAIL)
        self.recipe("two", RETAIL)
        with self.assertRaisesRegex(ValueError, "SCPH1001"):
            self.gate.check(self.root)

    def test_the_second_program_is_checked_too(self):
        self.recipe("one", RETAIL)
        self.recipe("two", '[runtime]\nopenbios=false\n[recompiler]\nbios_config="psxrecomp/bios/SCPH5552.toml"\n')
        self.asset("SCPH1001.toml")
        with self.assertRaisesRegex(ValueError, "SCPH5552"):
            self.gate.check(self.root)
        self.asset("SCPH5552.toml")
        self.assertEqual(len(self.gate.check(self.root)), 2)

    def test_a_program_that_allows_openbios_requires_its_assets(self):
        self.recipe("one", RETAIL)
        self.recipe("two", '[runtime]\n')
        self.asset("SCPH1001.toml")
        with self.assertRaisesRegex(ValueError, "OpenBIOS"):
            self.gate.check(self.root)
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            self.asset(name)
        self.assertEqual(len(self.gate.check(self.root)), 4)

    def test_a_program_without_a_recipe_is_named(self):
        self.recipe("one", RETAIL)
        self.asset("SCPH1001.toml")
        with self.assertRaisesRegex(ValueError, "programs/two/game.toml"):
            self.gate.check(self.root)

    def test_a_profile_outside_the_stage_fails(self):
        self.recipe("one", RETAIL)
        self.recipe("two", '[runtime]\nopenbios=false\n[recompiler]\nbios_config="../external.toml"\n')
        self.asset("SCPH1001.toml")
        with self.assertRaisesRegex(ValueError, "inside"):
            self.gate.check(self.root)

    def test_a_broken_set_file_names_its_key(self):
        (self.root / "set.toml").write_text(SET_TOML.replace('exe_name = "Program_Two"\n', ""))
        self.recipe("one", RETAIL)
        self.recipe("two", RETAIL)
        with self.assertRaisesRegex(ValueError, r"\[program\.two\] exe_name"):
            self.gate.check(self.root)

    def test_a_root_recipe_still_wins(self):
        # A single-program stage is checked exactly as before, set file or not.
        (self.root / "game.toml").write_text(
            '[runtime]\nopenbios=false\n[recompiler]\nbios_config="psxrecomp/bios/SCPH5552.toml"\n')
        self.asset("SCPH5552.toml")
        self.assertEqual(self.gate.check(self.root), [str(Path("psxrecomp/bios/SCPH5552.toml"))])



class GateTestsWithoutParser(GateTests):
    gate = gate_without_parser


class SetGateTestsWithoutParser(SetGateTests):
    gate = gate_without_parser


# Recipes for the reader comparison: those above, the packager test's, the two in
# tools/, and the forms a hand-written recipe uses (comments, single quotes,
# spacing, other keys and tables around the two the gate reads).
SAMPLES = {
    "retail": RETAIL,
    "openbios default": '[game]\nname = "x"\n',
    "openbios true, comment": '[runtime]\nopenbios = true   # the free BIOS\n',
    "single quotes, spaces": "[ recompiler ]\n  bios_config   =   'psxrecomp/bios/SCPH5552.toml'\n[runtime]\nopenbios=false\n",
    "other keys and tables": ('[game]\nname = "A # B"\ndiscs = [\n  "disc/1.cue",\n  "disc/2.cue",\n]\n'
                              '[recompiler]\nbios_config = "psxrecomp/bios/SCPH1001.toml"  # retail\nthreads = 4\n'
                              '[runtime]\nopenbios = false\noverlay_cache = true\nscale = 1.5\n'
                              '[runtime.mods]\nwide = true\n'),
    "packager test recipe": ('[game]\nname = "Resident Evil 2 (one)"\nid = "SLUS-00421"\nexe = "disc/BOOT"\n'
                             'discs = ["disc/disc-1.cue"]\ndisc_serials = ["SLUS-00421"]\n\n[recompiler]\n'
                             'bios_config = "psxrecomp/bios/SCPH1001.toml"\n\n[runtime]\nopenbios = false\n'
                             'overlay_cache = true\n'),
}
for _name in ("tools/bios_resume_testrom/game.toml", "tools/cycle_testrom/game.toml"):
    SAMPLES[_name] = (ROOT / _name).read_text(encoding="utf-8")


class ReaderAgreementTests(unittest.TestCase):
    """The fallback reader gives the values tomllib gives for what the gate reads."""

    def setUp(self):
        if gate.tomllib is None:
            self.skipTest("SKIP: no tomllib or tomli on this Python, so there is nothing to compare the "
                          "fallback reader with (the cases above ran with the fallback)")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()

    def test_both_readers_give_the_same_values(self):
        self.assertIsNone(gate_without_parser.tomllib)
        for name, text in SAMPLES.items():
            with self.subTest(recipe=name):
                path = self.root / "game.toml"
                path.write_text(text, encoding="utf-8")
                full, subset = gate.load_recipe(path), gate_without_parser.load_recipe(path)
                for table, key in (("runtime", "openbios"), ("recompiler", "bios_config")):
                    self.assertEqual(full.get(table, {}).get(key), subset.get(table, {}).get(key), (table, key))
                self.assertEqual(gate.recipe_assets(self.root, path),
                                 gate_without_parser.recipe_assets(self.root, path))



class FallbackStopTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()

    def test_the_fallback_stops_on_a_form_it_cannot_read(self):
        """A dotted key or an inline table for [runtime] or [recompiler] stops the
        fallback reader instead of being read as absent."""
        for text in ('runtime.openbios = false\n', 'recompiler = { bios_config = "x.toml" }\n',
                     '"runtime".openbios = false\n'):
            with self.subTest(recipe=text):
                path = self.root / "game.toml"
                path.write_text(text, encoding="utf-8")
                if gate.tomllib is not None:
                    self.assertIsNotNone(gate.load_recipe(path))   # a full parser reads it
                with self.assertRaisesRegex(ValueError, "inside its \\[table\\]"):
                    gate_without_parser.load_recipe(path)


if __name__ == "__main__":
    unittest.main()
