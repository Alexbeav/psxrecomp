#!/usr/bin/env python3
"""A set whose discs boot different programs, set up as one folder (PS1B-333).

tools/program_set.py holds the set file, the disc check, the folder links, the
join, the start scripts, the host project and the set's generate and rebuild.
These tests use small folders in place of real builds: no disc, no compiler.

The last group is the control for every other project: a config without a
[set] table never reaches the set's code, in the CLI or in update_disc_set.py.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tools"), str(ROOT / "tools" / "new_project_layout")]

import program_set as ps  # noqa: E402

SET_TOML = '''# written by the packager
[set]
name = "resident-evil-2-usa-dual-shock"
title = "Resident Evil 2"
exe_name = "Resident_Evil_2"
bios_stem = "SCPH1001"
serials = ["SLUS-00748", "SLUS-00756"]
programs = ["leon", "claire"]

[game]
discs = [
    "disc/disc-1.cue",   # placeholder until setup
    "disc/disc-2.cue",
]
disc_serials = ["SLUS-00748", "SLUS-00756"]

[program.leon]
folder = "programs/leon"
exe_name = "Resident_Evil_2_Leon"
shortcut = "Resident Evil 2 - Leon"
serials = ["SLUS-00748"]
positions = [1]

[program.claire]
folder = "programs/claire"
exe_name = "Resident_Evil_2_Claire"
shortcut = "Resident Evil 2 - Claire"
serials = ["SLUS-00756"]
positions = [2]
'''

GAME_TOML = '''[game]
name = "Resident Evil 2 ({who})"
id = "{serial}"
exe = "disc/{boot}"
disc = "disc/{who}.cue"
discs = ["disc/{who}.cue"]
disc_serials = ["{serial}"]
load_address = "0x80010000"

[runtime]
overlay_cache = true   # kept as written
'''

SERIAL_OF = {"d1.cue": "SLUS-00748", "d2.cue": "SLUS-00756", "other.cue": "SLUS-99999"}


def fake_probe(path):
    return SERIAL_OF.get(Path(path).name, "")


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))
    return path


def make_set(root: Path, text: str = SET_TOML) -> dict:
    write(root / "set.toml", text)
    for who, serial, boot in (("leon", "SLUS-00748", "SLUS_007.48"), ("claire", "SLUS-00756", "SLUS_007.56")):
        write(root / "programs" / who / "game.toml", GAME_TOML.format(who=who, serial=serial, boot=boot))
        write(root / "programs" / who / "CMakeLists.txt", "project(x)\n")
    for tree in ("psxrecomp", "recomp-ui"):
        write(root / tree / "marker.txt", tree)
    return ps.load_set(root / "set.toml")


def make_discs(folder: Path) -> list:
    return [write(folder / name, "cue") for name in ("d1.cue", "d2.cue")]


def make_product(folder: Path, who: str, serial: str, boot: str, exe: str, *, leftovers: bool = False) -> Path:
    """What one program's build folder holds after rebuild."""
    write(folder / (exe + ".exe"), "exe of " + who)
    write(folder / "game.toml", GAME_TOML.format(who=who, serial=serial, boot=boot))
    write(folder / "game_options.toml", "options\n")
    write(folder / "bios.cfg", "bios\n")
    write(folder / "disc.cfg", who + "\n")
    write(folder / "overlay_codegen_hash.h", "#define H 1\n")
    write(folder / "psx_game_version.txt", "0.4.0\n")
    write(folder / "SDL3.dll", "dll")
    write(folder / "assets" / "fonts" / "a.ttf", "font")
    write(folder / "mods" / "README.md", "mods of " + who)
    write(folder / "mods" / "bundled" / "psx.x" / "manifest.toml", "m")
    write(folder / "mods" / "state.toml", "this machine's mod state")
    write(folder / "overlay_toolchain" / "psxrecomp-game.exe", "emitter built for " + who)
    write(folder / "overlay_toolchain" / "tcc" / "examples" / "ex1.c", "int main;")
    write(folder / "overlay_toolchain" / "include" / "x.h", "h")
    write(folder / "saves" / "disc_digests.tsv", serial + "\tdigest\n")
    if who == "leon":
        write(folder / "settings.toml", "[video]\nscale = 2\n\n[disc]\npath = \"x\"\n\n[input]\npad = 1\n")
    if leftovers:
        write(folder / "CMakeCache.txt", "cache")
        write(folder / "CMakeFiles" / "x.o", "obj")
        write(folder / "build.ninja", "ninja")
    return folder


class SetFile(unittest.TestCase):
    def test_reads_the_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec = make_set(Path(tmp))
            self.assertEqual(spec["serials"], ["SLUS-00748", "SLUS-00756"])
            self.assertEqual(spec["discs"], ["disc/disc-1.cue", "disc/disc-2.cue"])
            self.assertEqual([p["program"] for p in spec["programs"]], ["leon", "claire"])
            self.assertEqual(spec["programs"][1]["positions"], [2])
            self.assertEqual(spec["bios_stem"], "SCPH1001")
            self.assertTrue(ps.is_set_config(Path(tmp) / "set.toml"))
            self.assertFalse(ps.is_set_config(Path(tmp) / "programs" / "leon" / "game.toml"))
            self.assertFalse(ps.is_set_config(Path(tmp) / "absent.toml"))

    def test_each_mistake_names_its_key(self):
        cases = [
            ('exe_name = "Resident_Evil_2"\n', "", "[set] exe_name"),
            ('title = "Resident Evil 2"\n', "", "[set] title"),
            ('programs = ["leon", "claire"]', 'programs = ["leon"]', "two or more programs"),
            ('disc_serials = ["SLUS-00748", "SLUS-00756"]', 'disc_serials = ["SLUS-00748"]', "[game] disc_serials"),
            ('    "disc/disc-2.cue",\n', "", "[game] discs"),
            ("[program.claire]", "[program.clair]", "[program.claire] is missing"),
            ('shortcut = "Resident Evil 2 - Claire"\n', "", "[program.claire] shortcut"),
            ('folder = "programs/claire"', 'folder = "../claire"', "[program.claire] folder"),
            ("positions = [2]", "positions = [1]", "disc 1 is SLUS-00756"),
            ('exe_name = "Resident_Evil_2_Claire"', 'exe_name = "Resident_Evil_2"', "different exe names"),
        ]
        for old, new, expect in cases:
            self.assertEqual(SET_TOML.count(old), 1, old)
            with tempfile.TemporaryDirectory() as tmp:
                path = write(Path(tmp) / "set.toml", SET_TOML.replace(old, new))
                with self.assertRaises(ps.SetError) as caught:
                    ps.load_set(path)
                self.assertIn(expect, str(caught.exception), (old, str(caught.exception)))

    def test_a_disc_no_program_boots_is_refused(self):
        text = SET_TOML.replace('serials = ["SLUS-00748", "SLUS-00756"]\nprograms',
                                'serials = ["SLUS-00748", "SLUS-00756", "SLUS-00999"]\nprograms')
        text = text.replace('    "disc/disc-2.cue",\n', '    "disc/disc-2.cue",\n    "disc/disc-3.cue",\n')
        text = text.replace('disc_serials = ["SLUS-00748", "SLUS-00756"]',
                            'disc_serials = ["SLUS-00748", "SLUS-00756", "SLUS-00999"]')
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ps.SetError) as caught:
                ps.load_set(write(Path(tmp) / "set.toml", text))
            self.assertIn("no program boots disc 3", str(caught.exception))


class Discs(unittest.TestCase):
    def test_every_disc_is_required_and_named(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec = make_set(Path(tmp) / "set")
            d1, d2 = make_discs(Path(tmp) / "discs")
            with self.assertRaises(ps.SetError) as caught:
                ps.check_discs(spec, ps.located_discs(spec, {1: str(d1)}), fake_probe)
            message = str(caught.exception)
            for part in ("Disc 2", "SLUS-00756", "claire", "every disc"):
                self.assertIn(part, message)
            self.assertNotIsInstance(caught.exception, ps.WrongDisc)
            ps.check_discs(spec, ps.located_discs(spec, {1: str(d1), 2: str(d2)}), fake_probe)

    def test_a_disc_in_the_wrong_position_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec = make_set(Path(tmp) / "set")
            d1, d2 = make_discs(Path(tmp) / "discs")
            with self.assertRaises(ps.WrongDisc) as caught:
                ps.check_discs(spec, [d2, d1], fake_probe)
            self.assertIn("Disc 1 of Resident Evil 2 must be SLUS-00748", str(caught.exception))
            self.assertIn("d2.cue is SLUS-00756", str(caught.exception))
            other = write(Path(tmp) / "discs" / "unknown.cue", "x")
            with self.assertRaises(ps.WrongDisc) as caught:
                ps.check_discs(spec, [d1, other], fake_probe)
            self.assertIn("not a disc this setup can identify", str(caught.exception))

    def test_where_the_discs_come_from(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            spec = make_set(root)
            d1, d2 = make_discs(Path(tmp) / "discs")
            self.assertEqual(ps.located_discs(spec), [None, None])       # placeholders are not discs
            self.assertEqual(ps.parse_set_disc_args(["1=a.cue", "2=b c.cue"]), {1: "a.cue", 2: "b c.cue"})
            with self.assertRaises(ps.SetError):
                ps.parse_set_disc_args(["a.cue"])
            # the wizard's disc.cfg: line N is disc N
            write(root / "disc.cfg", f"{d1}\n{d2}\n")
            self.assertEqual(ps.located_discs(spec), [d1.resolve(), d2.resolve()])
            (root / "disc.cfg").unlink()
            # what the last generate recorded
            write(root / "generated" / ps.SET_MARKER, json.dumps({"discs": [str(d1), str(d2)]}))
            self.assertEqual(ps.located_discs(spec), [d1.resolve(), d2.resolve()])
            # the command line wins
            other = write(Path(tmp) / "discs" / "other.cue", "x")
            self.assertEqual(ps.located_discs(spec, {2: str(other)}), [d1.resolve(), other.resolve()])

    def test_the_wizard_route_and_set_disc_locate_the_same_set(self):
        """The wizard runs update_disc_set.py and then `generate --disc <boot
        disc>`; a headless caller passes --set-disc. Both must end with the
        same discs in the same positions."""
        import update_disc_set
        with tempfile.TemporaryDirectory() as tmp:
            d1, d2 = make_discs(Path(tmp) / "discs")
            headless = make_set(Path(tmp) / "headless")
            by_flag = ps.located_discs(headless, ps.parse_set_disc_args([f"1={d1}", f"2={d2}"]))

            wizard_root = Path(tmp) / "wizard"
            make_set(wizard_root)
            real, ps.probe_serial = ps.probe_serial, fake_probe
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    code = update_disc_set.update_program_set(ps, wizard_root / "set.toml", [d1, d2], check=False)
            finally:
                ps.probe_serial = real
            self.assertEqual(code, 0)
            wizard = ps.load_set(wizard_root / "set.toml")
            self.assertEqual(wizard["discs"], [str(d1), str(d2)])
            self.assertEqual(wizard["serials"], headless["serials"])     # only [game] discs was written
            self.assertEqual(ps.located_discs(wizard, {1: str(d1)}), by_flag)
            self.assertEqual(by_flag, [d1.resolve(), d2.resolve()])


class UpdateDiscSet(unittest.TestCase):
    def run_tool(self, set_toml, discs, check=False):
        import update_disc_set
        real, ps.probe_serial = ps.probe_serial, fake_probe
        out, err = io.StringIO(), io.StringIO()
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = update_disc_set.update_program_set(ps, set_toml, discs, check=check)
        finally:
            ps.probe_serial = real
        return code, out.getvalue(), err.getvalue()

    def test_writes_only_the_disc_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            d1, d2 = make_discs(Path(tmp) / "discs")
            code, out, _ = self.run_tool(root / "set.toml", [d1, d2])
            self.assertEqual(code, 0, out)
            after = (root / "set.toml").read_text(encoding="utf-8").replace("\r\n", "\n")
            kept = [line for line in SET_TOML.splitlines() if "disc/disc-" not in line]
            for line in kept:
                self.assertIn(line, after.splitlines(), line)
            self.assertEqual(ps.load_set(root / "set.toml")["discs"], [str(d1), str(d2)])
            self.assertEqual(self.run_tool(root / "set.toml", [d1, d2], check=True)[0], 0)

    def test_refuses_a_part_of_the_set_and_a_wrong_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            before = (root / "set.toml").read_bytes()
            d1, d2 = make_discs(Path(tmp) / "discs")
            code, _, err = self.run_tool(root / "set.toml", [d1])
            self.assertEqual(code, 1)
            self.assertIn("has 2 discs and 1 were given", err)
            code, _, err = self.run_tool(root / "set.toml", [d2, d1])
            self.assertEqual(code, 1)
            self.assertIn("must be SLUS-00748", err)
            self.assertIn("nothing written", err)
            self.assertEqual((root / "set.toml").read_bytes(), before)


class Links(unittest.TestCase):
    def test_a_program_reaches_the_shared_trees(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            spec = make_set(root)
            made = ps.prepare_program_folder(spec, spec["programs"][0])
            folder = root / "programs" / "leon"
            self.assertEqual((folder / "psxrecomp" / "marker.txt").read_text(), "psxrecomp")
            self.assertEqual((folder / "recomp-ui" / "marker.txt").read_text(), "recomp-ui")
            self.assertIn(made["psxrecomp"], ("junction", "symlink", "copy"))
            # the download cache is shared: a file one program fetched is there for the next
            write(folder / ".cache" / "overlay-toolchain" / "tcc.zip", "archive")
            ps.prepare_program_folder(spec, spec["programs"][1])
            other = root / "programs" / "claire" / ".cache" / "overlay-toolchain" / "tcc.zip"
            if made[ps.DOWNLOAD_CACHE] != "copy":
                self.assertEqual(other.read_text(), "archive")
            again = ps.prepare_program_folder(spec, spec["programs"][0])
            if made["psxrecomp"] != "copy":
                self.assertEqual(again["psxrecomp"], "present")

    def test_a_moved_set_gets_fresh_links_and_an_own_tree_is_left_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            spec = make_set(root)
            folder = root / "programs" / "leon"
            elsewhere = Path(tmp) / "old" / "psxrecomp"
            write(elsewhere / "marker.txt", "old place")
            if ps.link_folder(folder / "psxrecomp", elsewhere) != "copy":
                self.assertEqual(ps.link_folder(folder / "psxrecomp", root / "psxrecomp"),
                                 "junction" if os.name == "nt" else "symlink")
                self.assertEqual((folder / "psxrecomp" / "marker.txt").read_text(), "psxrecomp")
                self.assertEqual((elsewhere / "marker.txt").read_text(), "old place")   # the target is untouched
            own = root / "programs" / "claire" / "recomp-ui"
            write(own / "marker.txt", "the program's own")
            self.assertEqual(ps.link_folder(own, root / "recomp-ui"), "own")
            self.assertEqual((own / "marker.txt").read_text(), "the program's own")
            with self.assertRaises(ps.SetError):
                ps.link_folder(folder / "x", root / "absent")
            del spec


class Join(unittest.TestCase):
    def products(self, tmp, leftovers=False):
        root = Path(tmp) / "set"
        spec = make_set(root)
        discs = make_discs(Path(tmp) / "discs")
        products = {
            "leon": make_product(root / "programs" / "leon" / "build-release", "leon", "SLUS-00748",
                                 "SLUS_007.48", "Resident_Evil_2_Leon", leftovers=leftovers),
            "claire": make_product(root / "programs" / "claire" / "build-release", "claire", "SLUS-00756",
                                   "SLUS_007.56", "Resident_Evil_2_Claire", leftovers=leftovers),
        }
        return root, spec, discs, products

    def test_one_folder_with_every_program(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, spec, discs, products = self.products(tmp, leftovers=True)
            out = root / "build-release"
            record = ps.join_products(spec, products, out, discs=discs)
            names = sorted(p.name for p in out.iterdir())
            self.assertEqual(names, sorted([
                "Resident_Evil_2_Leon.exe", "Resident_Evil_2_Leon.game.toml",
                "Resident_Evil_2_Claire.exe", "Resident_Evil_2_Claire.game.toml",
                "SDL3.dll", "assets", "bios.cfg", "game_options.toml", "mods", "overlay_codegen_hash.h",
                "overlay_toolchain", "psx_game_version.txt", "saves", "settings.toml"]))
            # no plain game.toml, no per-program leftovers, no build files, no tcc examples, no mod state
            for absent in ("game.toml", "disc.cfg", "CMakeCache.txt", "CMakeFiles", "build.ninja",
                           "overlay_toolchain/tcc/examples", "mods/state.toml"):
                self.assertFalse((out / absent).exists(), absent)
            self.assertEqual((out / "Resident_Evil_2_Claire.exe").read_text(), "exe of claire")
            # each config: the whole set, the program's own discs, every other line kept
            for exe, positions, who in (("Resident_Evil_2_Leon", [1], "leon"), ("Resident_Evil_2_Claire", [2], "claire")):
                text = (out / (exe + ".game.toml")).read_text(encoding="utf-8")
                game = ps.parse_toml_subset(text)["game"]
                self.assertEqual(game["discs"], [str(d) for d in discs])
                self.assertEqual(game["disc_serials"], ["SLUS-00748", "SLUS-00756"])
                self.assertEqual(game["program_discs"], positions)
                self.assertNotIn("disc", game)
                self.assertEqual(game["name"], f"Resident Evil 2 ({who})")
                self.assertIn("overlay_cache = true   # kept as written", text)
            # shared settings: every table but [disc]
            settings = (out / "settings.toml").read_text()
            self.assertIn("[video]", settings)
            self.assertIn("[input]", settings)
            self.assertNotIn("[disc]", settings)
            # known per-build files: the first program's copy, every hash recorded
            self.assertEqual((out / "mods" / "README.md").read_text(), "mods of leon")
            self.assertEqual(sorted(record["known_variants"]),
                             ["mods/README.md", "overlay_toolchain/psxrecomp-game.exe"])
            self.assertEqual(set(record["known_variants"]["mods/README.md"]["sha256"]), {"leon", "claire"})
            self.assertEqual((out / "saves" / "disc_digests.tsv").read_text().splitlines(),
                             ["SLUS-00748\tdigest", "SLUS-00756\tdigest"])
            self.assertEqual(record["line_unions"], ["saves/disc_digests.tsv"])
            self.assertEqual([row["program_discs"] for row in record["per_program"]], [[1], [2]])
            self.assertEqual(record["set_discs"], [str(d) for d in discs])
            self.assertEqual(record["not_taken"]["leon"], ["CMakeCache.txt", "CMakeFiles", "build.ninja"])
            for key in ("known_variants", "line_unions", "per_program", "shared_files", "set_discs"):
                self.assertIn(key, record)                                   # the keys Studio reads

    def test_a_clean_product_leaves_nothing_untaken_and_names_its_own_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, spec, _discs, products = self.products(tmp)
            record = ps.join_products(spec, products, Path(tmp) / "out")     # no discs given: Studio's call
            self.assertEqual(record["not_taken"], {"leon": [], "claire": []})
            self.assertEqual(record["set_discs"], ["disc/leon.cue", "disc/claire.cue"])

    def test_a_disagreement_stops_before_the_set_looks_installed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, spec, discs, products = self.products(tmp)
            write(products["claire"] / "game_options.toml", "other options\n")
            out = root / "build-release"
            with self.assertRaises(ps.SetError) as caught:
                ps.join_products(spec, products, out, discs=discs)
            self.assertIn("disagree about game_options.toml (leon and claire)", str(caught.exception))
            self.assertFalse((out / "Resident_Evil_2_Leon.exe").exists())   # what setup forwards to
            self.assertFalse((out / "Resident_Evil_2_Claire.exe").exists())

    def test_every_disagreement_is_reported_in_one_stop(self):
        """Rival Schools' first set build stopped on one file after 18 minutes
        of builds; whether a second file disagreed was unknown (PS1B-333)."""
        with tempfile.TemporaryDirectory() as tmp:
            root, spec, discs, products = self.products(tmp)
            write(products["claire"] / "game_options.toml", "other options\n")
            write(products["leon"] / "licenses" / "kit" / "THIRD_PARTY_NOTICES.md", "United by Fate\n")
            write(products["claire"] / "licenses" / "kit" / "THIRD_PARTY_NOTICES.md", "Evolution Disc\n")
            write(products["claire"] / "assets" / "fonts" / "a.ttf", "another font")
            # a file only one program carries, the known per-build files and the
            # line-union file are not disagreements
            write(products["claire"] / "assets" / "img" / "only-claire.tga", "x")
            out = root / "build-release"
            with self.assertRaises(ps.Disagreement) as caught:
                ps.join_products(spec, products, out, discs=discs)
            self.assertEqual([row["path"] for row in caught.exception.files],
                             ["assets/fonts/a.ttf", "game_options.toml", "licenses/kit/THIRD_PARTY_NOTICES.md"])
            for row in caught.exception.files:
                self.assertEqual(list(row["sha256"]), ["leon", "claire"])
                self.assertNotEqual(row["sha256"]["leon"], row["sha256"]["claire"])
            message = str(caught.exception)
            self.assertIn("disagree about 3 files: assets/fonts/a.ttf (leon and claire); game_options.toml "
                          "(leon and claire); licenses/kit/THIRD_PARTY_NOTICES.md (leon and claire).", message)
            self.assertNotIn("\n", message)
            self.assertFalse(out.exists())          # found before anything is written
            self.assertIsInstance(caught.exception, ps.SetError)

            record = Path(tmp) / "record.json"
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                code = ps.main(["join", "--set", str(root / "set.toml"),
                                "--program", f"leon={products['leon']}", "--program", f"claire={products['claire']}",
                                "--out", str(out), "--record", str(record)])
            self.assertEqual(code, 1)
            self.assertEqual(len(err.getvalue().strip().splitlines()), 1)
            self.assertEqual([row["path"] for row in json.loads(record.read_text())["disagreements"]],
                             ["assets/fonts/a.ttf", "game_options.toml", "licenses/kit/THIRD_PARTY_NOTICES.md"])
            self.assertFalse(out.exists())

    def test_three_programs_name_every_carrier(self):
        with tempfile.TemporaryDirectory() as tmp:
            folders = {}
            for name, text in (("a", "one"), ("b", "one"), ("c", "two")):
                folders[name] = Path(tmp) / name
                write(folders[name] / "game_options.toml", text)
                write(folders[name] / "bios.cfg", "same")
            spec = {"programs": [{"program": name} for name in ("a", "b", "c")]}
            found = ps.disagreements(spec, folders)
            self.assertEqual([row["path"] for row in found], ["game_options.toml"])
            self.assertEqual(list(found[0]["sha256"]), ["a", "b", "c"])
            self.assertIn("game_options.toml (a and b and c)", str(ps.Disagreement(found)))

    def test_installing_again_keeps_the_players_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, spec, discs, products = self.products(tmp)
            out = root / "build-release"
            ps.join_products(spec, products, out, discs=discs)
            write(out / "saves" / "card1.mcd", "save")
            write(out / "settings.toml", "[video]\nscale = 4\n")
            write(out / "game.toml", "one program's config, left by an older install\n")
            write(products["leon"] / "Resident_Evil_2_Leon.exe", "exe of leon, rebuilt")
            ps.join_products(spec, products, out, discs=discs)
            self.assertEqual((out / "saves" / "card1.mcd").read_text(), "save")
            self.assertEqual((out / "settings.toml").read_text(), "[video]\nscale = 4\n")
            self.assertEqual((out / "Resident_Evil_2_Leon.exe").read_text(), "exe of leon, rebuilt")
            self.assertFalse((out / "game.toml").exists())
            self.assertEqual((out / "saves" / "disc_digests.tsv").read_text().count("SLUS-00748"), 1)

    def test_a_program_that_was_not_built_is_named(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, spec, discs, products = self.products(tmp)
            (products["claire"] / "Resident_Evil_2_Claire.exe").unlink()
            with self.assertRaises(ps.SetError) as caught:
                ps.join_products(spec, products, root / "build-release", discs=discs)
            self.assertIn("Resident_Evil_2_Claire was not built", str(caught.exception))
            self.assertFalse((root / "build-release").exists())
            with self.assertRaises(ps.SetError) as caught:
                ps.join_products(spec, {"leon": products["leon"]}, root / "build-release", discs=discs)
            self.assertIn("claire", str(caught.exception))

    def test_command_line_join_writes_the_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, _spec, discs, products = self.products(tmp)
            record = Path(tmp) / "record.json"
            with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
                code = ps.main(["join", "--set", str(root / "set.toml"),
                                "--program", f"leon={products['leon']}", "--program", f"claire={products['claire']}",
                                "--set-disc", f"1={discs[0]}", "--set-disc", f"2={discs[1]}",
                                "--out", str(Path(tmp) / "out"), "--record", str(record)])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(record.read_text())["shared_files"], 11)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = ps.main(["join", "--set", str(root / "set.toml"), "--program", "leon",
                                "--out", str(Path(tmp) / "out2")])
            self.assertEqual(code, 1)
            self.assertEqual(len(err.getvalue().strip().splitlines()), 1)    # one line, non-zero exit


class KitCheck(unittest.TestCase):
    """The join's rule applied to the kits of a set, before any build."""

    def kits(self, tmp):
        kits = {}
        for name, title in (("arcade", "United by Fate"), ("evolution", "Evolution Disc")):
            kit = Path(tmp) / name
            write(kit / "game.toml", f'[game]\nname = "{title}"\n')            # each program's own
            write(kit / "seeds" / "funcs.txt", title)
            write(kit / "README.md", title)
            write(kit / "game_options.toml", "options\n")
            write(kit / "VERSION", "0.4.0\n")
            write(kit / "LICENSE", "GPL\n")
            write(kit / "THIRD_PARTY_NOTICES.md", f"covers the {title} project\n")
            write(kit / "mods" / "preloaded" / "packages" / "x.fix" / "1.0" / "manifest.toml", "id = \"x.fix\"\n")
            write(kit / "build-release" / "game_options.toml", "left by a build of " + title)
            write(kit / "psxrecomp" / "LICENSE", "the framework's, in " + title)
            kits[name] = kit
        return kits

    def test_names_the_files_that_would_stop_the_join(self):
        with tempfile.TemporaryDirectory() as tmp:
            kits = self.kits(tmp)
            result = ps.compare_kits(kits)
            self.assertEqual([row["path"] for row in result["shared"]], ["THIRD_PARTY_NOTICES.md"])
            self.assertIn("licenses/kit", result["shared"][0]["why"])
            self.assertEqual(list(result["shared"][0]["sha256"]), ["arcade", "evolution"])
            self.assertEqual(result["own"], ["README.md", "game.toml", "seeds/funcs.txt"])
            # every kind of kit file that reaches the shared folder
            write(kits["evolution"] / "game_options.toml", "other\n")
            write(kits["evolution"] / "VERSION", "0.5.0\n")
            write(kits["evolution"] / "mods" / "preloaded" / "packages" / "x.fix" / "1.0" / "manifest.toml", "other")
            write(kits["arcade"] / "launcher_assets" / "img" / "boxart.tga", "arcade art")
            write(kits["evolution"] / "launcher_assets" / "img" / "boxart.tga", "evolution art")
            self.assertEqual([row["path"] for row in ps.compare_kits(kits)["shared"]],
                             ["THIRD_PARTY_NOTICES.md", "VERSION", "game_options.toml",
                              "launcher_assets/img/boxart.tga", "mods/preloaded/packages/x.fix/1.0/manifest.toml"])

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            kits = self.kits(tmp)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = ps.main(["kit-check", f"arcade={kits['arcade']}", str(kits["evolution"])])
            self.assertEqual(code, 1)
            self.assertIn("differs: THIRD_PARTY_NOTICES.md (arcade and evolution)", out.getvalue())
            self.assertIn("1 file(s) the programs would disagree about", out.getvalue())
            write(kits["evolution"] / "THIRD_PARTY_NOTICES.md", "covers the United by Fate project\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = ps.main(["kit-check", "--json", str(kits["arcade"]), str(kits["evolution"])])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(out.getvalue())["shared"], [])
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(ps.main(["kit-check", str(kits["arcade"])]), 1)
                self.assertEqual(ps.main(["kit-check", str(kits["arcade"]), str(Path(tmp) / "absent")]), 1)
            self.assertIn("two or more", err.getvalue())
            self.assertIn("not a kit folder", err.getvalue())


class StartScripts(unittest.TestCase):
    RECORD = {"per_program": [
        {"shortcut": "Resident Evil 2 - Leon", "executable": "Resident_Evil_2_Leon.exe"},
        {"shortcut": "Resident Evil 2: Claire?", "executable": "Resident_Evil_2_Claire.exe"}]}

    def test_one_per_program_named_as_the_player_knows_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = ps.write_start_scripts({}, self.RECORD, root, root / "build-release", platform="windows")
            self.assertEqual(names, ["Resident Evil 2 - Leon.cmd", "Resident Evil 2 Claire.cmd"])
            text = (root / names[0]).read_bytes().decode("utf-8")
            self.assertIn('start "" /D "%~dp0build-release" "%~dp0build-release\\Resident_Evil_2_Leon.exe"', text)
            self.assertIn("\r\n", text)
            self.assertNotIn(str(root), text)                 # relative: the folder can be moved
            self.assertIn("exit /b 0", text)                  # start returns at once; no console stays
            self.assertNotIn("pause\r\nexit /b 0", text)

    def test_linux_and_macos_get_their_own(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            record = {"per_program": [{"shortcut": "Resident Evil 2 - Leon", "executable": "Resident_Evil_2_Leon"}]}
            linux = ps.write_start_scripts({}, record, root, root / "build-release", platform="linux")
            macos = ps.write_start_scripts({}, record, root, root / "build-release", platform="macos")
            self.assertEqual(linux, ["Resident Evil 2 - Leon.sh"])
            self.assertEqual(macos, ["Resident Evil 2 - Leon.command"])
            text = (root / linux[0]).read_bytes().decode("utf-8")
            self.assertTrue(text.startswith("#!/bin/sh\n"))
            self.assertNotIn("\r", text)
            self.assertIn('exe="$folder/Resident_Evil_2_Leon"', text)
            self.assertIn('folder="$here/build-release"', text)
            if os.name != "nt":
                self.assertTrue(os.access(str(root / linux[0]), os.X_OK))


class HostProject(unittest.TestCase):
    def test_init_host_writes_the_setup_project(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_set(root)
            self.assertEqual(ps.init_host(root), list(ps.HOST_FILES))
            cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
            setup = (root / "codegen_setup.c").read_text(encoding="utf-8")
            for text in (cmake, setup, (root / "codegen_setup.h").read_text(encoding="utf-8")):
                self.assertIn(ps.HOST_STAMP, text)
                self.assertNotRegex(text, r"@[A-Z_]+@")
            self.assertIn('EXE_NAME "Resident_Evil_2"', cmake)
            self.assertIn('GEN_MARKER "generated/program_set.generated"', cmake)
            self.assertIn('DEFAULT_GAME_CONFIG_PATH "set.toml"', cmake)
            self.assertIn("PSXRECOMP_FORCE_SETUP_HOST ON", cmake)
            self.assertIn('set(PSXRECOMP_BIOS_STEMS "SCPH1001"', cmake)
            self.assertIn('"${PSXRECOMP_ROOT}/bios/SCPH1001.toml"', cmake)
            # what the host checks: the set's marker, and the first program's exe in build-release
            self.assertIn('.seed_cfg_relpath = "set.toml"', setup)
            self.assertIn('.game_toml_relpath = "set.toml"', setup)
            self.assertIn('.gen_marker_relpath = "generated/program_set.generated"', setup)
            self.assertIn('.exe_basename = "Resident_Evil_2_Leon"', setup)
            self.assertIn('.build_dir_name = "build-release"', setup)
            self.assertEqual(ps.init_host(root), list(ps.HOST_FILES))      # its own files are replaced

    def test_init_host_keeps_a_file_it_did_not_write_and_validates_the_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_set(root)
            write(root / "CMakeLists.txt", "project(someone_elses)\n")
            with self.assertRaises(ps.SetError):
                ps.init_host(root)
            self.assertEqual((root / "CMakeLists.txt").read_text(), "project(someone_elses)\n")
            ps.init_host(root, force=True)
            self.assertIn(ps.HOST_STAMP, (root / "CMakeLists.txt").read_text())
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root / "set.toml", SET_TOML.replace('exe_name = "Resident_Evil_2"\n', 'exe_name = "Resident Evil 2"\n'))
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(ps.main(["init-host", "--root", str(root)]), 1)
            self.assertIn("[set] exe_name", err.getvalue())
            self.assertEqual(len(err.getvalue().strip().splitlines()), 1)
            self.assertFalse((root / "CMakeLists.txt").exists())

    def test_no_bios_stem_means_no_bios_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_set(root, SET_TOML.replace('bios_stem = "SCPH1001"\n', ""))
            ps.init_host(root)
            self.assertNotIn("PSXRECOMP_BIOS_STEM", (root / "CMakeLists.txt").read_text(encoding="utf-8"))

    def test_folders_for_the_packager(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_set(Path(tmp))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ps.main(["folders", "--set", str(Path(tmp) / "set.toml")]), 0)
            self.assertEqual(out.getvalue().split(), ["programs/leon", "programs/claire"])


class Progress:
    json_progress = False

    def __init__(self):
        self.events = []

    def phase(self, name, *, pct=None, message=None, **fields):
        self.events.append(("phase", name, pct, message))

    def log(self, message, *, level="info"):
        self.events.append(("log", message))

    def result(self, **fields):
        self.events.append(("result", fields))

    def error(self, message, *, code=1, **fields):
        self.events.append(("error", message, code, fields))

    def event(self, event, **fields):
        self.events.append((event, fields))

    def errors(self):
        return [e for e in self.events if e[0] == "error"]


def fake_cli(root: Path, *, fail_rebuild_of: str = "", calls=None):
    """The psxrecomp_cli module as the set's steps use it."""
    calls = calls if calls is not None else []

    def cmd_generate(args, progress):
        calls.append(("generate", vars(args).copy()))
        progress.phase("emit", pct=0.5, message="emitting")
        progress.result(ok=True, marker=str(Path(args.project_root) / "generated" / "x_dispatch.c"), disc=args.disc)
        return 0

    def cmd_rebuild(args, progress):
        calls.append(("rebuild", vars(args).copy(),
                      (root / "build-release" / "Resident_Evil_2_Leon.exe").exists()))
        who = Path(args.project_root).name
        if who == fail_rebuild_of:
            progress.error("link failed", code=1)
            return 1
        serial, boot = {"leon": ("SLUS-00748", "SLUS_007.48"), "claire": ("SLUS-00756", "SLUS_007.56")}[who]
        make_product(Path(args.build_dir), who, serial, boot, args.exe_basename, leftovers=True)
        progress.phase("done", pct=1.0, message="Rebuild complete")
        progress.result(ok=True, exe=str(Path(args.build_dir) / (args.exe_basename + ".exe")), lto=True)
        return 0

    def ensure_emitters(project_root, progress, *, download_toolchain=True, force=False):
        calls.append(("emitters", {"project_root": str(project_root), "download_toolchain": download_toolchain,
                                   "force": force}))

    return types.SimpleNamespace(
        EXIT_OK=0, EXIT_ERROR=1, EXIT_USAGE=2, EXIT_VERIFY=3, calls=calls,
        activate_embedded_toolchain=lambda root, progress: True,
        ensure_chd_reader=lambda root, progress: None,
        ensure_framework=lambda root, progress=None: Path(root) / "psxrecomp",
        ensure_emitters=ensure_emitters,
        cmd_generate=cmd_generate, cmd_rebuild=cmd_rebuild,
        _resolve_under=lambda base, raw: (Path(raw) if Path(raw).is_absolute() else Path(base) / raw).resolve())


def generate_args(root, **changes):
    args = argparse.Namespace(config=str(root / "set.toml"), project_root=str(root), disc="", set_disc=[],
                              bios="bios/SCPH1001.BIN", bios_stem="SCPH1001", gen_marker=ps.SET_MARKER,
                              force_bios=False, skip_hash_check=False, force_prepare=False,
                              no_toolchain_download=True, json_progress=True)
    for key, value in changes.items():
        setattr(args, key, value)
    return args


def rebuild_args(root, **changes):
    args = argparse.Namespace(config=str(root / "set.toml"), project_root=str(root), build_dir="build-release",
                              target="psx-runtime", exe_basename="Resident_Evil_2_Leon", disc="", set_disc=[],
                              no_pgo=True, force_pgo=False, diagnostic_dir="", diagnostic_only=False,
                              prune_after="build-intermediates", cmake_extra=["-DPSX_DEPS_OFFLINE=ON"],
                              no_toolchain_download=True, json_progress=True)
    for key, value in changes.items():
        setattr(args, key, value)
    return args


class SetSteps(unittest.TestCase):
    def setUp(self):
        self.real_probe, ps.probe_serial = ps.probe_serial, fake_probe

    def tearDown(self):
        ps.probe_serial = self.real_probe

    def test_generate_refuses_a_part_of_the_set_before_any_work(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            d1, _ = make_discs(Path(tmp) / "discs")
            write(root / "generated" / ps.SET_MARKER, "{}")
            cli, progress = fake_cli(root), Progress()
            code = ps.generate_set(cli, generate_args(root, disc=str(d1)), progress)   # the wizard's --disc
            self.assertEqual(code, cli.EXIT_USAGE)
            self.assertEqual(cli.calls, [])
            self.assertIn("Disc 2 of Resident Evil 2 is not located", progress.errors()[0][1])
            self.assertFalse((root / "programs" / "leon" / "psxrecomp").exists())

    def test_generate_refuses_a_wrong_disc_as_a_verify_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            d1, d2 = make_discs(Path(tmp) / "discs")
            cli, progress = fake_cli(root), Progress()
            code = ps.generate_set(cli, generate_args(root, set_disc=[f"1={d2}", f"2={d1}"]), progress)
            self.assertEqual(code, cli.EXIT_VERIFY)
            self.assertEqual(cli.calls, [])
            self.assertTrue(progress.errors()[0][3].get("verify_failed"))

    def test_generate_runs_each_program_then_writes_the_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            d1, d2 = make_discs(Path(tmp) / "discs")
            cli, progress = fake_cli(root), Progress()
            code = ps.generate_set(cli, generate_args(root, set_disc=[f"1={d1}", f"2={d2}"], force_emitters=True),
                                   progress)
            self.assertEqual(code, 0, progress.events)
            # the emitters once, from the set's root, before any program; never per program
            self.assertEqual([c[0] for c in cli.calls], ["emitters", "generate", "generate"])
            self.assertEqual(cli.calls[0][1], {"project_root": str(root.resolve()), "download_toolchain": False,
                                               "force": True})
            leon, claire = cli.calls[1][1], cli.calls[2][1]
            self.assertFalse(leon["force_emitters"])
            self.assertFalse(claire["force_emitters"])
            self.assertEqual(Path(leon["project_root"]), root / "programs" / "leon")
            self.assertEqual(Path(leon["config"]), root / "programs" / "leon" / "game.toml")
            self.assertEqual(Path(leon["disc"]), d1.resolve())
            self.assertEqual(Path(claire["disc"]), d2.resolve())
            self.assertEqual(leon["gen_marker"], "")                    # each program's own dispatch name
            self.assertEqual(Path(leon["bios"]), (root / "bios" / "SCPH1001.BIN").resolve())
            self.assertTrue(leon["no_toolchain_download"])              # the caller's flags reach each program
            self.assertEqual(leon["bios_stem"], "SCPH1001")
            self.assertTrue((root / "programs" / "claire" / "psxrecomp" / "marker.txt").is_file())
            marker = json.loads((root / "generated" / ps.SET_MARKER).read_text())
            self.assertEqual(marker["discs"], [str(d1.resolve()), str(d2.resolve())])
            self.assertEqual([p["program"] for p in marker["programs"]], ["leon", "claire"])
            phases = [e for e in progress.events if e[0] == "phase"]
            self.assertEqual(phases[-1][1:3], ("done", 1.0))
            self.assertEqual(sum(1 for e in phases if e[1] == "done"), 1)
            self.assertIn("leon (1 of 2): emitting", [e[3] for e in phases])
            pcts = [e[2] for e in phases if e[2] is not None]
            self.assertEqual(pcts, sorted(pcts))
            self.assertEqual(sum(1 for e in progress.events if e[0] == "result"), 1)

    def test_generate_reports_each_programs_split_prepass(self):
        """The set's result carries each program's pre-pass fields; the marker file does not (PS1B-135)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            d1, d2 = make_discs(Path(tmp) / "discs")
            cli, progress = fake_cli(root), Progress()
            plain = cli.cmd_generate
            fields = {"leon": {"prepass_passes": 208, "prepass_cap": 256, "prepass_converged": True},
                      "claire": {"prepass_passes": 256, "prepass_cap": 256, "prepass_converged": False}}

            def cmd_generate(args, child):
                code = plain(args, child)
                child.result(**child.last_result, **fields[Path(args.project_root).name])
                return code

            cli.cmd_generate = cmd_generate
            code = ps.generate_set(cli, generate_args(root, set_disc=[f"1={d1}", f"2={d2}"]), progress)
            self.assertEqual(code, 0, progress.events)
            result = [e[1] for e in progress.events if e[0] == "result"][0]
            self.assertEqual([p["program"] for p in result["programs"]], ["leon", "claire"])
            self.assertEqual([{key: p[key] for key in fields["leon"]} for p in result["programs"]],
                             [fields["leon"], fields["claire"]])
            marker = json.loads((root / "generated" / ps.SET_MARKER).read_text())
            self.assertEqual([sorted(p) for p in marker["programs"]], [["disc", "marker", "program"]] * 2)

    def test_a_failed_generate_leaves_no_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            d1, d2 = make_discs(Path(tmp) / "discs")
            write(root / "generated" / ps.SET_MARKER, json.dumps({"discs": [str(d1), str(d2)]}))
            cli = fake_cli(root)
            cli.cmd_generate = lambda args, progress: 1 if Path(args.project_root).name == "claire" else 0
            code = ps.generate_set(cli, generate_args(root, set_disc=[f"1={d1}", f"2={d2}"]), Progress())
            self.assertEqual(code, 1)
            self.assertFalse((root / "generated" / ps.SET_MARKER).exists())

    def generated(self, tmp):
        root = Path(tmp) / "set"
        make_set(root)
        d1, d2 = make_discs(Path(tmp) / "discs")
        write(root / "generated" / ps.SET_MARKER, json.dumps({"discs": [str(d1), str(d2)]}))
        return root, d1, d2

    def test_rebuild_builds_each_program_then_joins(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, d1, d2 = self.generated(tmp)
            write(root / "build-release" / "Resident_Evil_2_Leon.exe", "an older install")
            cli, progress = fake_cli(root), Progress()
            code = ps.rebuild_set(cli, rebuild_args(root), progress)
            self.assertEqual(code, 0, progress.events)
            self.assertEqual([c[0] for c in cli.calls], ["rebuild", "rebuild"])
            # until the join ends, the first program's exe is absent: setup is what starts
            self.assertEqual([c[2] for c in cli.calls], [False, False])
            leon = cli.calls[0][1]
            self.assertEqual(Path(leon["build_dir"]), root / "programs" / "leon" / "build-release")
            self.assertEqual(leon["exe_basename"], "Resident_Evil_2_Leon")
            self.assertEqual(cli.calls[1][1]["exe_basename"], "Resident_Evil_2_Claire")
            self.assertEqual(leon["prune_after"], "build-intermediates")
            self.assertEqual(leon["cmake_extra"], ["-DPSX_DEPS_OFFLINE=ON"])
            self.assertTrue(leon["no_pgo"])
            # each program's rebuild is told where the package's root is: its
            # licenses/toolchain texts are there, not in the program's folder
            self.assertEqual(Path(leon["package_root"]).resolve(), root.resolve())
            self.assertEqual(Path(cli.calls[1][1]["package_root"]).resolve(), root.resolve())
            out = root / "build-release"
            self.assertEqual((out / "Resident_Evil_2_Leon.exe").read_text(), "exe of leon")
            self.assertTrue((out / "Resident_Evil_2_Claire.game.toml").is_file())
            self.assertFalse((out / "CMakeCache.txt").exists())
            record = json.loads((root / ps.INSTALL_RECORD).read_text())
            self.assertEqual(record["set_discs"], [str(d1.resolve()), str(d2.resolve())])
            self.assertEqual(len(record["start_scripts"]), 2)
            for name in record["start_scripts"]:
                self.assertTrue((root / name).is_file())
                self.assertTrue(name.startswith("Resident Evil 2 - "))
            result = [e for e in progress.events if e[0] == "result"]
            self.assertEqual(len(result), 1)
            self.assertEqual(Path(result[0][1]["exe"]), out / "Resident_Evil_2_Leon.exe")

    def test_a_failed_program_leaves_setup_as_what_starts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, _d1, _d2 = self.generated(tmp)
            write(root / "build-release" / "Resident_Evil_2_Leon.exe", "an older install")
            cli, progress = fake_cli(root, fail_rebuild_of="claire"), Progress()
            self.assertEqual(ps.rebuild_set(cli, rebuild_args(root), progress), 1)
            self.assertFalse((root / "build-release" / "Resident_Evil_2_Leon.exe").exists())
            self.assertFalse((root / ps.INSTALL_RECORD).exists())
            self.assertIn("claire (2 of 2): link failed", progress.errors()[0][1])

    def test_diagnostic_mode_and_pgo_are_refused_in_plain_words(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, _d1, _d2 = self.generated(tmp)
            write(root / "build-release" / "Resident_Evil_2_Leon.exe", "installed")
            for changes, expect in (({"diagnostic_only": True, "diagnostic_dir": "build-diagnostic", "prune_after": ""},
                                     "Diagnostic mode is not available"),
                                    ({"diagnostic_dir": "build-diagnostic"}, "Diagnostic mode is not available"),
                                    ({"force_pgo": True, "no_pgo": False}, "optimised (PGO) rebuild is not available")):
                cli, progress = fake_cli(root), Progress()
                self.assertEqual(ps.rebuild_set(cli, rebuild_args(root, **changes), progress), cli.EXIT_USAGE)
                self.assertEqual(cli.calls, [])
                self.assertIn(expect, progress.errors()[0][1])
                self.assertIn("separate programs", progress.errors()[0][1])
                # the installed set was not touched
                self.assertEqual((root / "build-release" / "Resident_Evil_2_Leon.exe").read_text(), "installed")

    def test_rebuild_needs_a_complete_generate_and_the_discs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "set"
            make_set(root)
            cli, progress = fake_cli(root), Progress()
            self.assertEqual(ps.rebuild_set(cli, rebuild_args(root), progress), cli.EXIT_USAGE)
            self.assertIn("run Generate first", progress.errors()[0][1])
            d1, d2 = make_discs(Path(tmp) / "discs")
            write(root / "generated" / ps.SET_MARKER, json.dumps({"discs": [str(d1), str(d2)]}))
            d2.unlink()
            progress = Progress()
            self.assertEqual(ps.rebuild_set(cli, rebuild_args(root), progress), cli.EXIT_USAGE)
            self.assertIn("Disc 2 of Resident Evil 2 is not where it was", progress.errors()[0][1])
            self.assertEqual(cli.calls, [])


class EveryOtherProject(unittest.TestCase):
    """A config without a [set] table never reaches the set's code. This is
    what keeps single-disc projects and continuation sets (one program over
    several discs: Metal Gear Solid, Final Fantasy VII) exactly as they were."""

    CONTINUATION = ('[game]\nname = "Final Fantasy VII"\nid = "SCUS-94163"\nexe = "disc/SCUS_941.63"\n'
                    'discs = [\n    "disc/disc-1.cue",\n    "disc/disc-2.cue",\n    "disc/disc-3.cue",\n]\n'
                    'disc_serials = ["SCUS-94163", "SCUS-94164", "SCUS-94165"]\n\n[runtime]\noverlay_cache = true\n')

    def test_the_cli_takes_its_own_path(self):
        import psxrecomp_cli as cli
        seen = []
        real = (ps.generate_set, ps.rebuild_set)
        ps.generate_set = lambda *a: seen.append("generate") or 0
        ps.rebuild_set = lambda *a: seen.append("rebuild") or 0
        try:
            with tempfile.TemporaryDirectory() as tmp:
                for name, text in (("single.toml", '[game]\nname = "x"\n'), ("game.toml", self.CONTINUATION)):
                    config = write(Path(tmp) / name, text)
                    self.assertIsNone(cli.program_set_tool(config))
                    progress = Progress()
                    args = argparse.Namespace(config=str(config), project_root=tmp, disc="", set_disc=[], bios="",
                                              bios_stem="", force_bios=False, skip_hash_check=False,
                                              force_prepare=False, gen_marker="", json_progress=True)
                    code = cli.cmd_generate(args, progress)
                    # the ordinary generate answered: it looked for the disc itself
                    self.assertNotEqual(code, 0)
                    self.assertEqual(seen, [])
                    self.assertTrue(progress.errors())
                    self.assertNotIn("set", progress.errors()[0][1].lower().split())
                set_config = write(Path(tmp) / "set.toml", SET_TOML)
                self.assertIs(cli.program_set_tool(set_config), ps)
                cli.cmd_generate(argparse.Namespace(config=str(set_config), project_root=tmp), Progress())
                cli.cmd_rebuild(argparse.Namespace(config=str(set_config), project_root=tmp), Progress())
                self.assertEqual(seen, ["generate", "rebuild"])
        finally:
            ps.generate_set, ps.rebuild_set = real

    def test_set_disc_is_an_option_of_generate_and_rebuild(self):
        import psxrecomp_cli as cli
        parser = cli.build_parser()
        args = parser.parse_args(["generate", "--config", "set.toml", "--set-disc", "1=a.cue", "--set-disc", "2=b.cue"])
        self.assertEqual(args.set_disc, ["1=a.cue", "2=b.cue"])
        self.assertEqual(parser.parse_args(["generate", "--disc", "a.cue"]).set_disc, [])
        self.assertEqual(parser.parse_args(["rebuild", "--build-dir", "b"]).set_disc, [])

    def test_update_disc_set_takes_its_own_path(self):
        import update_disc_set
        with tempfile.TemporaryDirectory() as tmp:
            config = write(Path(tmp) / "game.toml", self.CONTINUATION)
            self.assertIsNone(update_disc_set.program_set_tool(config))
            self.assertIs(update_disc_set.program_set_tool(write(Path(tmp) / "set.toml", SET_TOML)), ps)


if __name__ == "__main__":
    unittest.main(verbosity=1)
