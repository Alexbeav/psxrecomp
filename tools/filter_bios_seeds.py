#!/usr/bin/env python3
"""Derive a sibling-BIOS seed file from the reference SCPH1001 seed corpus.

A seed from the reference corpus is kept for the target ROM iff the 64-byte
window starting at the seed address is byte-identical in both ROM images.
This is the same rule the original SCPH5552/SCPH101 seed files were described
with ("filtered vs <model> ROM (64-byte window)"); it is re-implemented here
so the derived files can be regenerated deterministically whenever the
reference corpus changes (e.g. after private-corpus seed restorations).

Usage:
  python tools/filter_bios_seeds.py \
      --reference-seeds recompiler/seeds/phase2_ghidra_seeds.json \
      --reference-rom   <path to SCPH1001 ROM, SHA-256 71af94d1...> \
      --target-rom      <path to target ROM> \
      --target-name     SCPH5552 \
      --out             recompiler/seeds/phase2_ghidra_seeds_SCPH5552.json

No ROM bytes are written to the output; only addresses/labels/rationales and
the SHA-256 of both ROMs (for provenance) are recorded.
"""
import argparse
import hashlib
import json
import sys

ROM_BASE = 0xBFC00000
WINDOW = 64


def load_rom(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) != 0x80000:
        sys.exit(f"{path}: expected 512 KiB BIOS image, got {len(data)} bytes")
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference-seeds", required=True)
    ap.add_argument("--reference-rom", required=True)
    ap.add_argument("--target-rom", required=True)
    ap.add_argument("--target-name", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--window", type=int, default=WINDOW)
    args = ap.parse_args()

    with open(args.reference_seeds, encoding="utf-8") as fh:
        ref = json.load(fh)
    seeds = ref["seeds"] if isinstance(ref, dict) else ref
    excluded = ref.get("excluded", []) if isinstance(ref, dict) else []

    ref_rom = load_rom(args.reference_rom)
    tgt_rom = load_rom(args.target_rom)

    kept, dropped = [], []
    for s in seeds:
        addr = int(s["address"], 16)
        off = addr - ROM_BASE
        if off < 0 or off + args.window > len(ref_rom):
            dropped.append(s)
            continue
        if ref_rom[off:off + args.window] == tgt_rom[off:off + args.window]:
            kept.append(s)
        else:
            dropped.append(s)

    # keep reference-corpus order so diffs against the reference stay minimal
    out = {
        "schema": ref.get("schema", "psxrecomp phase2 seeds") if isinstance(ref, dict) else "psxrecomp phase2 seeds",
        "source": (
            f"{args.reference_seeds.replace(chr(92), '/').split('/')[-1]} filtered vs {args.target_name} ROM "
            f"({args.window}-byte window); reference ROM sha256 {hashlib.sha256(ref_rom).hexdigest()}; "
            f"target ROM sha256 {hashlib.sha256(tgt_rom).hexdigest()}; "
            f"kept {len(kept)} dropped {len(dropped)}"
        ),
        "seed_count": len(kept),
        "seeds": kept,
        "excluded": excluded,
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, indent=2)
        fh.write("\n")
    print(f"kept {len(kept)} dropped {len(dropped)} -> {args.out}")
    if dropped:
        lo = min(int(s["address"], 16) for s in dropped)
        print(f"lowest dropped address 0x{lo:08X}")


if __name__ == "__main__":
    main()
