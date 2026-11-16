#!/usr/bin/env python3
"""Enumerate BIOS callable boundaries and build the reference seed corpus.

The reference corpus (recompiler/seeds/phase2_ghidra_seeds.json, SCPH1001) used
to be merged from an ignored Ghidra export (generated/ghidra_function_starts.json)
by tools/gen_phase2_seeds.py, so nobody could regenerate it from a clean checkout.
This tool replaces that export. Every seed carries a `provenance`:

  vector        architectural R3000A entry (reset, BEV=1 general exception)
  kernel_table  target of the A0/B0/C0 kernel call tables. Table bounds are
                read from the kernel code itself (CopyA0Table's memcpy bounds,
                GetB0Table/GetC0Table return values; docs/psx_bios_disasm.txt)
                and verified before use.
  call_target   function entry that discovery (psxrecomp-bios --discover-only)
                reaches from the vector + kernel_table + post_return roots
  post_return   code nothing references: an uncovered run that starts right
                after a function's `jr`/`j` + delay slot and decodes as valid
                instructions up to its own `jr ra`
  manual        curated entry with a recorded rationale (dispatch-miss history)
  ghidra_legacy Ghidra function start no rule above proves; kept as-is

Inputs are all committed: the BIOS profile, the curated file
(recompiler/seeds/bios_curated_seeds.json) and the ROM image you supply.
`generate` writes the corpus; `generate --check` fails if the committed corpus
differs from what the inputs produce. `classify` reports what an address is
(function entry, block leader, mid-block, delay slot, uncovered).

Usage:
  python tools/bios_seed_corpus.py generate --profile bios/SCPH1001.toml \
      [--rom bios/SCPH1001.BIN] [--recompiler recompiler/build/psxrecomp-bios] \
      [--report out.md] [--check]
  python tools/bios_seed_corpus.py classify --profile bios/SCPH1001.toml \
      --addresses 0xBFC0C0B0,0xBFC098DC [--rom ...] [--recompiler ...]

No ROM bytes are written anywhere; outputs hold addresses, labels, rationales
and ROM SHA-256 only.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM_BASE = 0xBFC00000
ROM_PHYS = 0x1FC00000
ROM_SIZE = 0x80000
CURATED = os.path.join("recompiler", "seeds", "bios_curated_seeds.json")
SCHEMA = "psxrecomp phase2 seeds"

JR_RA = 0x03E00008
KERNEL_P1 = (0x00000, 0x10000)  # ROM offsets executed in place

VECTORS = [
    (0xBFC00000, "reset_vector", "MIPS R3000A reset vector"),
    (0xBFC00180, "general_exception_bev1", "BEV=1 general exception vector"),
]

PROVENANCE_ORDER = ["vector", "kernel_table", "manual", "call_target",
                    "post_return", "ghidra_legacy"]


# ---------------------------------------------------------------- decoding

_SPECIAL_OK = {0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 16, 17, 18, 19, 24, 25, 26, 27,
               32, 33, 34, 35, 36, 37, 38, 39, 42, 43}
_PRIMARY_OK = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18,
               32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 50, 58}


def valid_insn(w):
    """True if w decodes to an R3000A instruction the PS1 can execute."""
    op = w >> 26
    if op not in _PRIMARY_OK:
        return False
    if op == 0:
        return (w & 0x3F) in _SPECIAL_OK
    if op == 1:
        return ((w >> 16) & 0x1F) in (0, 1, 16, 17)
    return True


def is_transfer(w):
    """Branch or jump: the next word is its delay slot."""
    op = w >> 26
    if op == 0:
        return (w & 0x3F) in (8, 9)
    if op == 1:
        return ((w >> 16) & 0x1F) in (0, 1, 16, 17)
    return op in (2, 3, 4, 5, 6, 7)


def is_unconditional_end(w):
    """`jr <reg>` or `j`: control never falls through past the delay slot."""
    return (w >> 26 == 0 and (w & 0x3F) == 8) or (w >> 26 == 2)


def norm(addr):
    return (addr & 0x1FFFFFFF) - ROM_PHYS


def rom_addr(off):
    return ROM_BASE + off


# ---------------------------------------------------------------- profile

class Profile:
    def __init__(self, path, rom_override=None):
        with open(path, "rb") as fh:
            self.data = tomllib.load(fh)
        self.path = path
        prog = self.data["program"]
        self.rom_path = rom_override or os.path.join(ROOT, prog["rom"])
        self.sha256 = prog.get("image", {}).get("sha256", "")
        recomp = self.data["recompiler"]
        self.seeds_path = os.path.join(ROOT, recomp["seeds"])
        self.copies = []
        for c in recomp.get("address_model", {}).get("copy", []):
            lo = int(c["rom_lo"], 16) & 0x1FFFFFFF
            hi = int(c["rom_hi"], 16) & 0x1FFFFFFF
            self.copies.append((c["name"], lo - ROM_PHYS, hi - ROM_PHYS,
                                int(c["ram_lo"], 16) & 0x1FFFFFFF))

    def code_windows(self):
        wins = [("Kernel Part 1", KERNEL_P1[0], KERNEL_P1[1])]
        wins += [(n, lo, hi) for n, lo, hi, _ in self.copies]
        return wins

    def ram_to_rom(self, value):
        """ROM offset for a pointer into ROM or into a relocated copy window."""
        phys = value & 0x1FFFFFFF
        if ROM_PHYS <= phys < ROM_PHYS + ROM_SIZE:
            return phys - ROM_PHYS
        for _, lo, hi, ram in self.copies:
            if ram <= phys < ram + (hi - lo):
                return lo + (phys - ram)
        return None


def load_rom(path, sha256):
    with open(path, "rb") as fh:
        rom = fh.read()
    if len(rom) != ROM_SIZE:
        sys.exit(f"{path}: expected a 512 KiB BIOS image, got {len(rom)} bytes")
    digest = hashlib.sha256(rom).hexdigest()
    if sha256 and digest != sha256:
        sys.exit(f"{path}: SHA-256 {digest} does not match the profile's {sha256}")
    return rom, digest


def words_of(rom):
    return struct.unpack("<%dI" % (len(rom) // 4), rom)


# ---------------------------------------------------------------- kernel tables

def _lui_addiu_value(w_lui, w_add):
    lo = w_add & 0xFFFF
    if w_add >> 26 == 9 and lo & 0x8000:
        lo -= 0x10000
    return ((w_lui & 0xFFFF) << 16) + lo & 0xFFFFFFFF


def kernel_tables(prof, W):
    """A0/B0/C0 call tables, located from the kernel code that builds them.

    A0: CopyA0Table (ROM 0xBFC042D0) copies [a0, a1) = [0xBFC04300, 0xBFC04604)
        to RAM 0x200. B0/C0: GetB0Table (RAM 0x668) and GetC0Table (RAM 0x65C)
        return the RAM bases 0x874 and 0x674 of tables that live in Kernel
        Part 2. Entry counts are the documented kernel function ranges
        (A0 00h-C0h from the copy size, B0 00h-5Dh, C0 00h-1Dh); the words
        after each table are checked to be zero so a shifted layout fails loudly.
    """
    def at(off):
        return W[off // 4]

    # CopyA0Table: lui a0 / lui a1 / addiu a0 / addiu a1 / addiu a2,zero,0x200
    base = 0x42D0
    expect = [(0x3C04, None), (0x3C05, None), (0x2484, None), (0x24A5, None)]
    for i, (hi, _) in enumerate(expect):
        if at(base + 4 * i) >> 16 != hi:
            raise SystemExit("kernel layout check failed: CopyA0Table not at ROM 0xBFC042D0")
    a0_lo = _lui_addiu_value(at(base), at(base + 8))
    a0_hi = _lui_addiu_value(at(base + 4), at(base + 12))
    if at(base + 16) != 0x24060200:
        raise SystemExit("kernel layout check failed: CopyA0Table destination is not RAM 0x200")
    a0_count = (a0_hi - a0_lo) // 4

    def getter(ram_addr):
        off = prof.ram_to_rom(ram_addr)
        w_lui, w_jr, w_add = at(off), at(off + 4), at(off + 8)
        if w_lui >> 16 != 0x3C02 or w_jr != JR_RA or w_add >> 16 != 0x2442:
            raise SystemExit(f"kernel layout check failed: table getter at RAM 0x{ram_addr:X}")
        return _lui_addiu_value(w_lui, w_add)

    tables = [("A0", norm(a0_lo), a0_count),
              ("B0", prof.ram_to_rom(getter(0x668)), 0x5E),
              ("C0", prof.ram_to_rom(getter(0x65C)), 0x1E)]
    for name, off, count in tables:
        if name == "A0":
            continue
        if at(off + 4 * (count - 1)) == 0 or any(at(off + 4 * (count + i)) for i in range(2)):
            raise SystemExit(f"kernel layout check failed: {name} table extent at ROM 0x{rom_addr(off):08X}")
    return tables


def kernel_table_targets(prof, W):
    out = {}
    for name, off, count in kernel_tables(prof, W):
        for i in range(count):
            v = W[off // 4 + i]
            if v == 0:
                continue
            t = prof.ram_to_rom(v)
            if t is None or t % 4:
                continue
            out.setdefault(t, f"{name}:{i:02X}")
    return out


# ---------------------------------------------------------------- discovery

class Discovery:
    def __init__(self, manifest, unsupported):
        self.entries = set()
        self.leaders = set()
        self.covered = set()
        for fn in manifest["functions"]:
            e = norm(int(fn["entry_addr"], 16))
            end = norm(int(fn["end_addr"], 16))
            self.entries.add(e)
            self.leaders.update(norm(int(b, 16)) for b in fn["block_leaders"])
            if 0 <= e <= end < ROM_SIZE:
                self.covered.update(range(e, end + 4, 4))
        self.unsupported = {norm(int(u["address"], 16)) for u in unsupported}


def run_discovery(args, prof, seeds):
    with tempfile.TemporaryDirectory() as tmp:
        seed_file = os.path.join(tmp, "seeds.json")
        with open(seed_file, "w", encoding="utf-8") as fh:
            json.dump({"schema": SCHEMA, "seeds": [
                {"address": f"0x{rom_addr(o):08X}", "label": f"s_{o:05X}",
                 "rationale": "enumeration root"} for o in sorted(seeds)]}, fh)
        out = os.path.join(tmp, "out")
        cmd = [args.recompiler, "--config", prof.path, "--rom", prof.rom_path,
               "--seeds", seed_file, "--out-dir", out, "--discover-only"]
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        manifest_path = os.path.join(out, "function_manifest.json")
        if not os.path.exists(manifest_path):
            sys.exit("discovery failed:\n" + proc.stdout + proc.stderr)
        with open(manifest_path, encoding="utf-8") as fh:
            manifest = json.load(fh)
        with open(os.path.join(out, "unsupported_ops.json"), encoding="utf-8") as fh:
            unsupported = json.load(fh)
    return Discovery(manifest, unsupported)


def post_return_candidates(prof, W, disc):
    """Uncovered code runs that start after a function end and reach `jr ra`."""
    found = []
    for _, lo, hi in prof.code_windows():
        i = lo
        while i < hi:
            if i in disc.covered:
                i += 4
                continue
            j = i
            while j < hi and j not in disc.covered:
                j += 4
            k = i
            while k < j and W[k // 4] == 0:
                k += 4
            ok = k < j and k >= 8 and (k - 4) - 4 >= 0
            if ok:
                p = k - 4
                while p > lo and W[p // 4] == 0 and p >= i:
                    p -= 4
                # p is the delay slot of the preceding function's last transfer
                ok = p - 4 >= lo and is_unconditional_end(W[(p - 4) // 4]) and (p - 4) in disc.covered
            if ok:
                m = k
                ok = False
                while m + 4 < j:
                    w = W[m // 4]
                    if not valid_insn(w):
                        break
                    if w == JR_RA:
                        ok = valid_insn(W[(m + 4) // 4])
                        break
                    m += 4
            if ok:
                found.append(k)
            i = j
    return found


# ---------------------------------------------------------------- corpus

def load_curated():
    with open(os.path.join(ROOT, CURATED), encoding="utf-8") as fh:
        data = json.load(fh)
    return data


def seed_entry(off, label, rationale, provenance):
    return {"address": f"0x{rom_addr(off):08X}", "label": label,
            "rationale": rationale, "provenance": provenance}


def enumerate_corpus(args, prof, rom, W):
    curated = load_curated()
    cur = {}
    for s in curated["seeds"]:
        cur[norm(int(s["address"], 16))] = s

    tables = kernel_table_targets(prof, W)
    roots = {norm(a) for a, _, _ in VECTORS} | set(tables)

    # Coverage for the post_return scan uses every seed we will ship, so a run
    # is only "unreferenced" if nothing in the final corpus reaches it.
    post = set()
    baseline_unsupported = None
    while True:
        full = run_discovery(args, prof, roots | post | set(cur))
        if baseline_unsupported is None:
            baseline_unsupported = full.unsupported
        new = [k for k in post_return_candidates(prof, W, full) if k not in post]
        if not new:
            break
        post.update(new)
    if full.unsupported - baseline_unsupported:
        extra = ", ".join(f"0x{rom_addr(o):08X}" for o in sorted(full.unsupported - baseline_unsupported))
        sys.exit(f"post_return seeds reached unsupported instructions: {extra}")

    proven = run_discovery(args, prof, roots | post)

    seeds = {}
    for a, label, rat in VECTORS:
        seeds[norm(a)] = seed_entry(norm(a), label, rat, "vector")
    for off, slot in sorted(tables.items()):
        if off in seeds:
            continue
        c = cur.get(off)
        label = c["label"] if c and c.get("provenance") == "manual" else f"ktab_{rom_addr(off):08X}"
        seeds[off] = seed_entry(off, label, f"kernel call table {slot} target", "kernel_table")
    for off, c in sorted(cur.items()):
        if off in seeds:
            continue
        if c["provenance"] == "manual":
            seeds[off] = seed_entry(off, c["label"], c["rationale"], "manual")
    for off in sorted(post):
        if off in seeds:
            continue
        seeds[off] = seed_entry(off, f"post_return_{rom_addr(off):08X}",
                                "unreferenced code run after a function end, valid up to jr ra",
                                "post_return")
    for off in sorted(proven.entries):
        if off in seeds or not (0 <= off < ROM_SIZE):
            continue
        seeds[off] = seed_entry(off, f"fn_{rom_addr(off):08X}",
                                "function entry reached by discovery from vector/kernel_table/post_return roots",
                                "call_target")
    for off, c in sorted(cur.items()):
        if off in seeds:
            continue
        seeds[off] = seed_entry(off, c["label"], c["rationale"], c["provenance"])

    corpus = {
        "schema": SCHEMA,
        "source": ("tools/bios_seed_corpus.py generate over "
                   f"{os.path.basename(prof.path)} (ROM sha256 {prof.sha256 or '-'}) "
                   f"+ {CURATED.replace(os.sep, '/')}"),
        "seed_count": len(seeds),
        "seeds": [seeds[o] for o in sorted(seeds)],
        "excluded": curated.get("excluded", []),
    }
    stats = {
        "tables": tables, "post_return": sorted(post), "full": full,
        "curated": cur,
    }
    return corpus, stats


def dump_json(obj):
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"


# ---------------------------------------------------------------- classify

def classify_offsets(W, disc, offsets):
    rows = []
    for off in offsets:
        prev = W[off // 4 - 1] if off >= 4 else 0
        if off in disc.entries:
            cls = "function_entry"
        elif off in disc.leaders:
            cls = "block_leader"
        elif off not in disc.covered:
            cls = "uncovered"
        elif is_transfer(prev) and (off - 4) in disc.covered:
            cls = "delay_slot"
        else:
            cls = "mid_block"
        rows.append((off, cls))
    return rows


# ---------------------------------------------------------------- commands

def default_recompiler():
    exe = os.path.join(ROOT, "recompiler", "build", "psxrecomp-bios")
    return exe + ".exe" if os.name == "nt" else exe


def cmd_generate(args):
    prof = Profile(os.path.join(ROOT, args.profile) if not os.path.isabs(args.profile) else args.profile,
                   args.rom)
    rom, _ = load_rom(prof.rom_path, prof.sha256)
    W = words_of(rom)
    corpus, stats = enumerate_corpus(args, prof, rom, W)
    text = dump_json(corpus)
    if args.check:
        with open(prof.seeds_path, encoding="utf-8") as fh:
            committed = fh.read()
        if committed != text:
            sys.exit(f"{prof.seeds_path} is stale: regenerate with "
                     f"`python tools/bios_seed_corpus.py generate --profile {args.profile}`")
        print(f"OK {prof.seeds_path} matches ({corpus['seed_count']} seeds)")
    else:
        with open(prof.seeds_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print(f"wrote {prof.seeds_path} ({corpus['seed_count']} seeds)")
    counts = {}
    for s in corpus["seeds"]:
        counts[s["provenance"]] = counts.get(s["provenance"], 0) + 1
    print("provenance: " + ", ".join(f"{p}={counts.get(p, 0)}" for p in PROVENANCE_ORDER))
    if args.report:
        write_report(args.report, prof, W, corpus, stats)


def write_report(path, prof, W, corpus, stats):
    full = stats["full"]
    lines = [f"# BIOS seed corpus report — {os.path.basename(prof.path)}", "",
             f"Corpus: {corpus['seed_count']} seeds. Source: {corpus['source']}", ""]
    prov = {norm(int(s["address"], 16)): s["provenance"] for s in corpus["seeds"]}
    cur = stats["curated"]
    lines += ["## Kernel call table targets", "",
              "| slot | address | seed provenance | in curated input |", "|---|---|---|---|"]
    for off, slot in sorted(stats["tables"].items(), key=lambda kv: kv[1]):
        lines.append(f"| {slot} | 0x{rom_addr(off):08X} | {prov.get(off, '-')} | "
                     f"{cur[off]['provenance'] if off in cur else 'no'} |")
    lines += ["", "## post_return seeds", ""]
    for off in stats["post_return"]:
        lines.append(f"- 0x{rom_addr(off):08X}")
    rows = classify_offsets(W, full, [norm(int(s['address'], 16)) for s in corpus["seeds"]])
    bad = [(o, c) for o, c in rows if c not in ("function_entry",)]
    lines += ["", "## Corpus entries that are not function entries after discovery", ""]
    lines += [f"- 0x{rom_addr(o):08X} {c}" for o, c in bad] or ["(none)"]
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"wrote {path}")


def cmd_classify(args):
    prof = Profile(os.path.join(ROOT, args.profile) if not os.path.isabs(args.profile) else args.profile,
                   args.rom)
    rom, _ = load_rom(prof.rom_path, prof.sha256)
    W = words_of(rom)
    with open(prof.seeds_path, encoding="utf-8") as fh:
        corpus = json.load(fh)
    disc = run_discovery(args, prof, {norm(int(s["address"], 16)) for s in corpus["seeds"]})
    offs = [norm(int(a, 16)) for a in args.addresses.split(",") if a.strip()]
    for off, cls in classify_offsets(W, disc, offs):
        print(f"0x{rom_addr(off):08X} {cls}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("generate", "classify"):
        p = sub.add_parser(name)
        p.add_argument("--profile", required=True)
        p.add_argument("--rom")
        p.add_argument("--recompiler", default=default_recompiler())
        if name == "generate":
            p.add_argument("--check", action="store_true")
            p.add_argument("--report")
        else:
            p.add_argument("--addresses", required=True)
    args = ap.parse_args()
    {"generate": cmd_generate, "classify": cmd_classify}[args.cmd](args)


if __name__ == "__main__":
    main()
