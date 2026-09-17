"""BIOS seed corpus consistency.

The structural checks need no ROM and run everywhere. The derivation checks
(`generate --check`, `filter_bios_seeds.py --check`, kernel identity) run when
the retail images are present under bios/ with the pinned SHA-256 (or under
PSXRECOMP_BIOS_DIR) and, for the SCPH1001 corpus, a built psxrecomp-bios;
otherwise they are reported as skipped.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import bios_seed_corpus  # noqa: E402

SEEDS = ROOT / 'recompiler' / 'seeds'
REFERENCE = SEEDS / 'phase2_ghidra_seeds.json'
CURATED = SEEDS / 'bios_curated_seeds.json'
DERIVED = {'SCPH5552': SEEDS / 'phase2_ghidra_seeds_SCPH5552.json',
           'SCPH5500': SEEDS / 'phase2_ghidra_seeds_SCPH5500.json'}
PROVENANCE = {'vector', 'kernel_table', 'call_target', 'post_return', 'manual', 'ghidra_legacy'}
SHELL = 0x1FC18000

# Addresses that August builds dispatched to and that are mid-block
# instructions or delay slots, not callable entries (T13/T110). Seeding them as
# function starts would hide whatever published those PCs.
NOT_ENTRIES = [0xBFC0192C, 0xBFC02B7C, 0xBFC04830, 0xBFC048B0, 0xBFC048F0, 0xBFC04A9C,
               0xBFC04BA4, 0xBFC04C18, 0xBFC04DCC, 0xBFC04F9C, 0xBFC0504C, 0xBFC05084,
               0xBFC06660, 0xBFC07814, 0xBFC07974, 0xBFC08620, 0xBFC098DC, 0xBFC0A554]

ROMS = {'SCPH1001': ('SCPH1001.BIN', '71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3'),
        'SCPH5552': ('EUR-PSX-SCPH5552.bin', '1faaa18fa820a0225e488d9f086296b8e6c46df739666093987ff7d8fd352c09'),
        'SCPH5500': ('SCPH5500.BIN', '9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb')}


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def phys(seed):
    return int(seed['address'], 16) & 0x1FFFFFFF


def rom(model):
    name, sha = ROMS[model]
    for base in filter(None, [os.environ.get('PSXRECOMP_BIOS_DIR'), str(ROOT / 'bios')]):
        path = Path(base) / name
        if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == sha:
            return path
    return None


class StructureTest(unittest.TestCase):
    def check_file(self, path):
        data = load(path)
        seeds = data['seeds']
        self.assertEqual(data['seed_count'], len(seeds), f'{path.name}: seed_count')
        addrs = [phys(s) for s in seeds]
        self.assertEqual(len(addrs), len(set(addrs)), f'{path.name}: duplicate address after KSEG normalization')
        self.assertEqual(addrs, sorted(addrs), f'{path.name}: not sorted by address')
        for s in seeds:
            with self.subTest(file=path.name, address=s['address']):
                self.assertEqual(phys(s) & 3, 0)
                self.assertTrue(0x1FC00000 <= phys(s) < 0x1FC80000)
                self.assertIn(s.get('provenance'), PROVENANCE)
                self.assertTrue(s.get('label'))
        self.assertIn(0x1FC00000, addrs)
        self.assertIn(0x1FC00180, addrs)
        for a in NOT_ENTRIES:
            self.assertNotIn(a & 0x1FFFFFFF, addrs, f'{path.name}: mid-block/delay-slot address 0x{a:08X} seeded')
        return data

    def test_reference_corpus(self):
        self.check_file(REFERENCE)

    def test_derived_corpora(self):
        ref = {phys(s): s for s in load(REFERENCE)['seeds']}
        for model, path in DERIVED.items():
            with self.subTest(model=model):
                data = self.check_file(path)
                for s in data['seeds']:
                    self.assertLess(phys(s), SHELL, f'{model}: shell seed {s["address"]}')
                    if s['provenance'] != 'kernel_table':
                        self.assertEqual(ref.get(phys(s)), s, f'{model}: {s["address"]} not from the reference corpus')

    def test_curated_entries_ship(self):
        ref = {phys(s): s for s in load(REFERENCE)['seeds']}
        curated = load(CURATED)
        for s in curated['seeds']:
            with self.subTest(address=s['address']):
                self.assertIn(s['provenance'], {'manual', 'ghidra_legacy'})
                self.assertIn(phys(s), ref)
                if s['provenance'] == 'ghidra_legacy':
                    self.assertEqual(ref[phys(s)], s)
        self.assertEqual(curated['excluded'], load(REFERENCE)['excluded'])


class DerivationTest(unittest.TestCase):
    def test_reference_corpus_regenerates(self):
        rom1001 = rom('SCPH1001')
        exe = bios_seed_corpus.default_recompiler()
        if rom1001 is None or not os.path.isfile(exe):
            self.skipTest('needs the SCPH1001 image and a built psxrecomp-bios')
        subprocess.run([sys.executable, str(ROOT / 'tools' / 'bios_seed_corpus.py'), 'generate',
                        '--profile', 'bios/SCPH1001.toml', '--rom', str(rom1001), '--check'],
                       cwd=ROOT, check=True, capture_output=True)

    def test_derived_corpora_regenerate(self):
        import filter_bios_seeds
        rom1001 = rom('SCPH1001')
        for model, path in DERIVED.items():
            with self.subTest(model=model):
                target = rom(model)
                if rom1001 is None or target is None:
                    self.skipTest(f'needs the SCPH1001 and {model} images')
                text, _, _, _ = filter_bios_seeds.derive_text(
                    str(REFERENCE), rom1001.read_bytes(), target.read_bytes(), model,
                    str(ROOT / 'bios' / f'{model}.toml'), str(target))
                self.assertEqual(path.read_text(encoding='utf-8'), text)

    def test_kernel_identity(self):
        rom1001 = rom('SCPH1001')
        if rom1001 is None:
            self.skipTest('needs the SCPH1001 image')
        ref = rom1001.read_bytes()
        for model, (lo, hi) in {'SCPH5552': (0x0, 0x18000), 'SCPH5500': (0x10000, 0x18000)}.items():
            with self.subTest(model=model):
                target = rom(model)
                if target is None:
                    self.skipTest(f'needs the {model} image')
                self.assertEqual(ref[lo:hi], target.read_bytes()[lo:hi])


if __name__ == '__main__':
    unittest.main()
