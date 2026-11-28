#!/usr/bin/env python3
"""Tests for tools/psyq_sigs: signature parsing, the matching levels, our own
signatures and the fetch extractor. All fixtures are synthetic MIPS words; no
SDK or game bytes are used."""

import importlib.util
import io
import json
import pathlib
import struct
import sys
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1] / "psyq_sigs"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


PS = _load("psyq_sigs")
FETCH = _load("fetch_signatures")

JR_RA = 0x03E00008
NOP = 0


def words(*ws: int) -> bytes:
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in ws)


def unique_code(seed: int, n: int) -> bytes:
    """n distinct ADDIU words (addiu t0, t1, imm) that never repeat across seeds."""
    return words(*[(0x09 << 26) | (9 << 21) | (8 << 16) | ((seed * 256 + i) & 0xFFFF) for i in range(n)])


def ps_exe(code: bytes, base: int = 0x80010000) -> bytes:
    hdr = bytearray(0x800)
    hdr[:8] = b"PS-X EXE"
    struct.pack_into("<II", hdr, 0x18, base, len(code))
    return bytes(hdr) + code


def sig_of(code: bytes, wild: set[int] = frozenset()) -> str:
    return " ".join("??" if i in wild else f"{b:02X}" for i, b in enumerate(code))


class Fixture:
    def __init__(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)

    def image(self, code: bytes) -> "PS.Image":
        p = self.dir / "GAME.EXE"
        p.write_bytes(ps_exe(code))
        return PS.Image.load(p, None)

    def sigset(self, version: str, lib: str, entries: list[dict]) -> pathlib.Path:
        root = self.dir / "sigs"
        (root / version).mkdir(parents=True, exist_ok=True)
        (root / version / f"{lib}.json").write_text(json.dumps(entries), encoding="utf-8")
        return root


class ParseTests(unittest.TestCase):
    def test_wildcards(self):
        pat, mask = PS.parse_sig_text("01 ?? FF")
        self.assertEqual(pat, b"\x01\x00\xff")
        self.assertEqual(mask, b"\xff\x00\xff")

    def test_local_labels_are_not_functions(self):
        s = PS.Signature("psyq", "460", "L", "O", b"\0" * 16, b"\xff" * 16,
                         [("DrawOTag", 0), ("loc_1C", 8), ("text_0", 0)])
        self.assertEqual(PS.function_labels(s), [("DrawOTag", 0)])


class MatchTests(unittest.TestCase):
    def setUp(self):
        self.fx = Fixture()
        self.addCleanup(self.fx.tmp.cleanup)

    def test_whole_object_names_every_label_and_merges_versions(self):
        obj = unique_code(1, 12)
        code = unique_code(9, 4) + obj + unique_code(8, 4)
        entry = {"name": "DRAW.OBJ", "sig": sig_of(obj, {4, 5}),
                 "labels": [{"name": "DrawOTag", "offset": 0}, {"name": "DrawPrim", "offset": 24}]}
        self.fx.sigset("440", "LIBGPU", [entry])
        root = self.fx.sigset("460", "LIBGPU", [entry])
        rep = PS.match_image(self.fx.image(code), PS.load_public_set(root), [])
        names = {f["name"]: f for f in rep["functions"]}
        self.assertEqual(names["DrawOTag"]["pc"], "0x80010010")
        self.assertEqual(names["DrawPrim"]["pc"], "0x80010028")
        self.assertEqual(names["DrawOTag"]["versions"], ["440", "460"])
        self.assertEqual(rep["by_source"]["psyq-obj"], 2)

    def test_ambiguous_pattern_is_rejected(self):
        obj = unique_code(2, 8)
        root = self.fx.sigset("460", "LIBGTE", [{"name": "A.OBJ", "sig": sig_of(obj),
                                                "labels": [{"name": "Twice", "offset": 0}]}])
        rep = PS.match_image(self.fx.image(obj + obj), PS.load_public_set(root), [])
        self.assertEqual(rep["functions_named"], 0)
        self.assertEqual(rep["rejected"].get("ambiguous"), 2)   # object and its slice

    def test_function_slice_when_object_differs(self):
        f1, f2 = unique_code(3, 10), unique_code(4, 10)
        obj_sig = sig_of(f1 + f2)
        # The game's copy of the object has a changed second function.
        code = f1 + unique_code(5, 10)
        root = self.fx.sigset("460", "LIBGPU", [{"name": "B.OBJ", "sig": obj_sig, "labels": [
            {"name": "First", "offset": 0}, {"name": "Second", "offset": 40}]}])
        rep = PS.match_image(self.fx.image(code), PS.load_public_set(root), [])
        self.assertEqual([(f["name"], f["how"]) for f in rep["functions"]], [("First", "psyq-fn")])

    def test_short_object_only_flush_against_a_placed_one(self):
        big = unique_code(6, 12)
        tiny = words(0x48C60000, JR_RA, NOP)       # ctc2 + return: matches anywhere
        code = tiny + unique_code(7, 8) + big + tiny
        root = self.fx.sigset("460", "LIBGTE", [
            {"name": "BIG.OBJ", "sig": sig_of(big), "labels": [{"name": "Big", "offset": 0}]},
            {"name": "TINY.OBJ", "sig": sig_of(tiny), "labels": [{"name": "SetTiny", "offset": 0}]},
        ])
        rep = PS.match_image(self.fx.image(code), PS.load_public_set(root), [])
        names = {f["name"]: f for f in rep["functions"]}
        self.assertEqual(names["SetTiny"]["how"], "psyq-near")
        self.assertEqual(names["SetTiny"]["pc"], f"0x{0x80010000 + len(code) - 12:08X}")

    def test_short_object_flush_at_two_sites_is_ambiguous(self):
        big = unique_code(6, 12)
        tiny = words(0x48C60000, JR_RA, NOP)
        code = tiny + big + tiny                   # flush on both sides of BIG
        root = self.fx.sigset("460", "LIBGTE", [
            {"name": "BIG.OBJ", "sig": sig_of(big), "labels": [{"name": "Big", "offset": 0}]},
            {"name": "TINY.OBJ", "sig": sig_of(tiny), "labels": [{"name": "SetTiny", "offset": 0}]},
        ])
        rep = PS.match_image(self.fx.image(code), PS.load_public_set(root), [])
        self.assertNotIn("SetTiny", {f["name"] for f in rep["functions"]})

    def test_claim_prefers_the_longer_pattern(self):
        def sig(n: int, concrete: int) -> "PS.Signature":
            mask = b"\xff" * concrete + b"\x00" * (n - concrete)
            return PS.Signature("psyq", "460", "L", f"O{n}", b"\x00" * n, mask, [])
        long_m = PS.Match(sig(64, 20), 0, set())
        short_m = PS.Match(sig(32, 32), 16, set())
        kept = PS.claim([short_m, long_m])
        self.assertEqual([m.sig.obj for m in kept], ["O64"])

    def test_toml_is_symbols_format(self):
        obj = unique_code(10, 10)
        root = self.fx.sigset("460", "LIBGPU", [{"name": "C.OBJ", "sig": sig_of(obj),
                                                "labels": [{"name": "ClearOTagR", "offset": 0}]}])
        toml = PS.render_toml(PS.match_image(self.fx.image(obj), PS.load_public_set(root), []))
        self.assertIn('name = "ClearOTagR"', toml)
        self.assertIn("pc = 0x80010000", toml)
        self.assertIn("emit = false", toml)


class GenTests(unittest.TestCase):
    def test_mask_relocations(self):
        code = words(
            0x0C004000,                  # jal
            0x3C048001,                  # lui a0, 0x8001
            0x24840010,                  # addiu a0, a0, 0x10  (lo half of a0)
            0x8F820020,                  # lw v0, 0x20(gp)
            0x24A50004,                  # addiu a1, a1, 4     (not relocated)
        )
        m = PS.mask_function(code)
        self.assertEqual(m[0:4], b"\x00\x00\x00\xfc")
        self.assertEqual(m[4:8], b"\x00\x00\xff\xff")
        self.assertEqual(m[8:12], b"\x00\x00\xff\xff")
        self.assertEqual(m[12:16], b"\x00\x00\xff\xff")
        self.assertEqual(m[16:20], b"\xff\xff\xff\xff")

    def test_load_clears_a_high_register(self):
        code = words(
            0x3C048001,                  # lui a0, 0x8001
            0x8C840010,                  # lw a0, 0x10(a0)  (lo half: masked; a0 now data)
            0x8C850004,                  # lw a1, 4(a0)     (a0 is data: concrete)
        )
        m = PS.mask_function(code)
        self.assertEqual(m[4:8], b"\x00\x00\xff\xff")
        self.assertEqual(m[8:12], b"\xff\xff\xff\xff")

    def test_gen_then_match_elsewhere(self):
        fx = Fixture()
        self.addCleanup(fx.tmp.cleanup)
        fn = unique_code(11, 10) + words(JR_RA, NOP)
        a = fx.dir / "A.EXE"
        a.write_bytes(ps_exe(fn + unique_code(12, 4)))
        names = fx.dir / "named.json"
        names.write_text(json.dumps([{"pc": "0x80010000", "name": "MyFunc"}]), encoding="utf-8")
        out = fx.dir / "gen.json"
        PS.main(["gen", "--exe", str(a), "--names", str(names), "--out", str(out)])
        doc = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(doc["format"], "psxrecomp-psyq-gen-1")
        self.assertEqual(len(doc["signatures"][0]["sig"].split()), len(fn))   # cut at jr ra + slot
        rep = PS.match_image(fx.image(unique_code(13, 6) + fn), [], PS.load_gen_set(out))
        self.assertEqual(rep["functions"], [{"pc": "0x80010018", "name": "MyFunc", "lib": "",
                                             "obj": "MyFunc", "how": "gen", "versions": []}])


class FetchTests(unittest.TestCase):
    def test_extract_keeps_only_version_json(self):
        with tempfile.TemporaryDirectory() as t:
            t = pathlib.Path(t)
            arc = t / "a.tar.gz"
            with tarfile.open(arc, "w:gz") as tf:
                for name, data in [("top/460/LIBGPU.json", b"[]"), ("top/README.md", b"x"),
                                   ("top/460/../../evil.json", b"[]"), ("top/tools/x.json", b"[]")]:
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tf.addfile(info, io.BytesIO(data))
            dest = t / "out"
            self.assertEqual(FETCH.extract(arc, dest), 1)
            self.assertTrue((dest / "460" / "LIBGPU.json").is_file())
            self.assertFalse((t / "evil.json").exists())

    def test_source_pin_is_complete(self):
        src = FETCH.load_source()
        self.assertEqual(len(src["commit"]), 40)
        self.assertEqual(len(src["archive_sha256"]), 64)
        self.assertIn(src["commit"], src["archive"])


if __name__ == "__main__":
    unittest.main()
