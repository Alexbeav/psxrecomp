#!/usr/bin/env python3
"""Derive a sibling-BIOS seed file from the reference SCPH1001 seed corpus.

A seed from the reference corpus is kept for the target ROM iff the 64-byte
window starting at the seed address is byte-identical in both ROM images.
This is the same rule the original SCPH5552/SCPH101 seed files were described
with ("filtered vs <model> ROM (64-byte window)"); it is re-implemented here
so the derived files can be regenerated deterministically whenever the
reference corpus changes (e.g. after private-corpus seed restorations).

Two kinds of seed do not depend on the window:
  - `vector` seeds (reset, BEV=1 exception) are architectural and always kept;
  - with --target-profile, the target ROM's own A0/B0/C0 kernel call table
    targets are enumerated (tools/bios_seed_corpus.py) and added as
    `kernel_table` seeds, so a kernel byte difference near a table target
    cannot silently drop it.

Usage:
  python tools/filter_bios_seeds.py \
      --reference-seeds recompiler/seeds/phase2_ghidra_seeds.json \
      --reference-rom   <path to SCPH1001 ROM, SHA-256 71af94d1...> \
      --target-rom      <path to target ROM> \
      --target-name     SCPH5552 \
      --target-profile  bios/SCPH5552.toml \
      --out             recompiler/seeds/phase2_ghidra_seeds_SCPH5552.json \
      [--check]

No ROM bytes are written to the output; only addresses/labels/rationales and
the SHA-256 of both ROMs (for provenance) are recorded.
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bios_seed_corpus  # noqa: E402

ROM_BASE = 0xBFC00000
WINDOW = 64


def load_rom(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) != 0x80000:
        sys.exit(f"{path}: expected 512 KiB BIOS image, got {len(data)} bytes")
    return data


def derive_text(reference_seeds, ref_rom, tgt_rom, target_name, target_profile=None,
                target_rom_path=None, window=WINDOW):
    """Return (json text, kept, dropped, added) for the derived seed file."""
    with open(reference_seeds, encoding="utf-8") as fh:
        ref = json.load(fh)
    seeds = ref["seeds"] if isinstance(ref, dict) else ref
    excluded = ref.get("excluded", []) if isinstance(ref, dict) else []

    kept, dropped = [], []
    for s in seeds:
        addr = int(s["address"], 16)
        off = addr - ROM_BASE
        if s.get("provenance") == "vector":
            kept.append(s)
            continue
        if off < 0 or off + window > len(ref_rom):
            dropped.append(s)
            continue
        if ref_rom[off:off + window] == tgt_rom[off:off + window]:
            kept.append(s)
        else:
            dropped.append(s)

    added = 0
    if target_profile:
        prof = bios_seed_corpus.Profile(target_profile, target_rom_path)
        words = bios_seed_corpus.words_of(tgt_rom)
        have = {int(s["address"], 16) & 0x1FFFFFFF for s in kept}
        for off, slot in sorted(bios_seed_corpus.kernel_table_targets(prof, words).items()):
            addr = ROM_BASE + off
            if addr & 0x1FFFFFFF in have:
                continue
            kept.append({"address": f"0x{addr:08X}", "label": f"ktab_{addr:08X}",
                         "rationale": f"kernel call table {slot} target ({target_name} ROM)",
                         "provenance": "kernel_table"})
            added += 1
        kept.sort(key=lambda s: int(s["address"], 16) & 0x1FFFFFFF)

    out = {
        "schema": ref.get("schema", "psxrecomp phase2 seeds") if isinstance(ref, dict) else "psxrecomp phase2 seeds",
        "source": (
            f"{reference_seeds.replace(chr(92), '/').split('/')[-1]} filtered vs {target_name} ROM "
            f"({window}-byte window); reference ROM sha256 {hashlib.sha256(ref_rom).hexdigest()}; "
            f"target ROM sha256 {hashlib.sha256(tgt_rom).hexdigest()}; "
            f"kept {len(kept) - added} dropped {len(dropped)}"
            + (f"; plus {added} {target_name} kernel_table targets" if target_profile else "")
        ),
        "seed_count": len(kept),
        "seeds": kept,
        "excluded": excluded,
    }
    return json.dumps(out, indent=2, ensure_ascii=False) + "\n", kept, dropped, added


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference-seeds", required=True)
    ap.add_argument("--reference-rom", required=True)
    ap.add_argument("--target-rom", required=True)
    ap.add_argument("--target-name", required=True)
    ap.add_argument("--target-profile")
    ap.add_argument("--out", required=True)
    ap.add_argument("--window", type=int, default=WINDOW)
    ap.add_argument("--check", action="store_true",
                    help="fail if --out differs from what would be written")
    args = ap.parse_args()

    text, kept, dropped, added = derive_text(
        args.reference_seeds, load_rom(args.reference_rom), load_rom(args.target_rom),
        args.target_name, args.target_profile, args.target_rom, args.window)
    if args.check:
        with open(args.out, encoding="utf-8") as fh:
            if fh.read() != text:
                sys.exit(f"{args.out} is stale: rerun tools/filter_bios_seeds.py without --check")
        print(f"OK {args.out} matches ({len(kept)} seeds)")
        return
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(f"kept {len(kept) - added} dropped {len(dropped)} added {added} -> {args.out}")
    if dropped:
        lo = min(int(s["address"], 16) for s in dropped)
        print(f"lowest dropped address 0x{lo:08X}")


if __name__ == "__main__":
    main()
