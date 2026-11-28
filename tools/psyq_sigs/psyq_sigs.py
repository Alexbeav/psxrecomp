#!/usr/bin/env python3
"""Name the Sony PsyQ SDK functions in a PS1 executable from byte signatures.

Two signature sources, used in this order:

1. The public PsyQ signature set (lab313ru/psx_psyq_signatures; pinned in
   SOURCE.toml, fetched by fetch_signatures.py, never committed here). Each
   entry is a whole library object (.OBJ) with wildcards over relocated
   bytes, plus the offsets of the functions inside it.
2. Our own signatures, written by ``gen`` from a binary whose functions are
   already named. They are per function, with every relocation-dependent
   field masked, and they fill the gaps: SDK objects the public set lacks,
   or objects a game's linker changed enough that the whole-object pattern
   no longer matches.

Matching is exact over the concrete bytes. A pattern that matches more than
one place is rejected as ambiguous, and so is any match that overlaps a
longer one, so a name is only given where one object can be there.

Commands::

    psyq_sigs.py match --sigs DIR --exe SLES_029.13 [--gen-sigs our.json]
                       [--toml out.toml] [--json out.json]
    psyq_sigs.py gen   --exe SLUS_005.94 --names named.json --out our.json

``match`` writes a symbols.toml fragment in the game-root format read by
tools/new_project_layout/sync_symbols.py ([[func]] pc/name/emit/status/note)
and a JSON report with per-library counts. ``gen`` reads the JSON report of
a previous ``match`` (or any JSON list of {"pc", "name"}) for the named
binary. Raw overlay files are accepted with ``--load-address``.

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import struct
import sys
from dataclasses import dataclass, field

LOCAL_LABEL = re.compile(r"^(loc|text|data|rdata|sdata|bss|sbss|lbl|jpt|def)_[0-9A-Fa-f]+$")
C_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
MIN_CONCRETE_OBJ = 16       # bytes of non-wildcard pattern for a whole object
MIN_CONCRETE_FN = 32        # bytes for a per-function slice or our own signature
MAX_GEN_BYTES = 512         # longest signature ``gen`` writes for one function
MIN_CONCRETE_NEAR = 8       # a short object placed only flush against a placed one
NEAR_GAP = 16               # bytes of alignment padding allowed between objects


# ---------------------------------------------------------------------------
# Executable image
# ---------------------------------------------------------------------------

@dataclass
class Image:
    path: pathlib.Path
    data: bytes            # the code/data that is loaded at ``base``
    base: int              # guest load address
    sha256: str
    index: dict[int, list[int]] = field(default_factory=dict)

    @classmethod
    def load(cls, path: pathlib.Path, load_address: int | None) -> "Image":
        raw = path.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        if raw[:8] == b"PS-X EXE":
            t_addr, t_size = struct.unpack_from("<II", raw, 0x18)
            data = raw[0x800:0x800 + t_size]
            base = t_addr
        else:
            if load_address is None:
                raise SystemExit(f"{path}: not a PS-X EXE; pass --load-address")
            data, base = raw, load_address
        img = cls(path, data, base & 0xFFFFFFFF, sha)
        for off in range(0, len(data) - 3, 4):
            w = struct.unpack_from("<I", data, off)[0]
            img.index.setdefault(w, []).append(off)
        return img


# ---------------------------------------------------------------------------
# Signatures
# ---------------------------------------------------------------------------

@dataclass
class Signature:
    source: str            # "psyq" or "gen"
    version: str           # SDK version directory ("460"), or "" for gen
    lib: str
    obj: str
    pattern: bytes
    mask: bytes            # 0xFF = concrete byte, 0x00 = wildcard
    labels: list[tuple[str, int]]
    versions: set[str] = field(default_factory=set)
    concrete: int = 0
    _regex: re.Pattern[bytes] | None = None

    def __post_init__(self) -> None:
        self.concrete = sum(1 for m in self.mask if m)

    def regex(self) -> re.Pattern[bytes]:
        if self._regex is None:
            parts = []
            for b, m in zip(self.pattern, self.mask):
                parts.append(re.escape(bytes([b])) if m else b".")
            self._regex = re.compile(b"".join(parts), re.DOTALL)
        return self._regex

    def anchor(self, img: Image) -> tuple[int, list[int]] | None:
        """Least frequent fully concrete aligned word, as (offset, hits)."""
        best: tuple[int, list[int]] | None = None
        for off in range(0, len(self.pattern) - 3, 4):
            if self.mask[off:off + 4] != b"\xff\xff\xff\xff":
                continue
            w = struct.unpack_from("<I", self.pattern, off)[0]
            hits = img.index.get(w, [])
            if best is None or len(hits) < len(best[1]):
                best = (off, hits)
                if not hits:
                    break
        return best

    def find(self, img: Image) -> list[int]:
        a = self.anchor(img)
        if a is None:
            return []
        aoff, hits = a
        rx = self.regex()
        out = []
        for h in hits:
            start = h - aoff
            if start < 0 or start + len(self.pattern) > len(img.data):
                continue
            if rx.match(img.data, start):
                out.append(start)
        return out


def parse_sig_text(text: str) -> tuple[bytes, bytes]:
    pat, mask = bytearray(), bytearray()
    for tok in text.split():
        if tok == "??":
            pat.append(0)
            mask.append(0)
        else:
            pat.append(int(tok, 16))
            mask.append(0xFF)
    return bytes(pat), bytes(mask)


def load_public_set(root: pathlib.Path) -> list[Signature]:
    """All objects of all SDK versions; identical patterns merged, versions kept."""
    sigs: dict[tuple[str, str, bytes, bytes], Signature] = {}
    for vdir in sorted(p for p in root.iterdir() if p.is_dir() and p.name.isdigit()):
        for f in sorted(vdir.glob("*.json")):
            lib = f.name[:-len(".json")]
            for entry in json.loads(f.read_text(encoding="utf-8")):
                if not entry.get("sig"):       # data-only objects carry no code pattern
                    continue
                pat, mask = parse_sig_text(entry["sig"])
                key = (lib, entry["name"], pat, mask)
                if key in sigs:
                    sigs[key].versions.add(vdir.name)
                    continue
                labels = [(l["name"], int(l["offset"])) for l in entry.get("labels", [])]
                sigs[key] = Signature("psyq", vdir.name, lib, entry["name"], pat, mask, labels,
                                      {vdir.name})
    return list(sigs.values())


def load_gen_set(path: pathlib.Path) -> list[Signature]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    sigs = []
    for e in doc["signatures"]:
        pat, mask = parse_sig_text(e["sig"])
        sigs.append(Signature("gen", "", e.get("lib", ""), e["name"], pat, mask,
                              [(e["name"], 0)]))
    return sigs


def function_labels(sig: Signature) -> list[tuple[str, int]]:
    return sorted(((n, o) for n, o in sig.labels if not LOCAL_LABEL.match(n)), key=lambda x: x[1])


def slice_functions(sig: Signature) -> list[Signature]:
    """Per-function sub-signatures of a whole-object signature."""
    fl = function_labels(sig)
    out = []
    for i, (name, off) in enumerate(fl):
        end = fl[i + 1][1] if i + 1 < len(fl) else len(sig.pattern)
        if end <= off:
            continue
        s = Signature("psyq-fn", sig.version, sig.lib, sig.obj,
                      sig.pattern[off:end], sig.mask[off:end], [(name, 0)], set(sig.versions))
        if s.concrete >= MIN_CONCRETE_FN:
            out.append(s)
    return out


# ---------------------------------------------------------------------------
# Matching
# ---------------------------------------------------------------------------

@dataclass
class Match:
    sig: Signature
    start: int             # offset in the image
    versions: set[str]

    @property
    def end(self) -> int:
        return self.start + len(self.sig.pattern)


def claim(matches: list[Match]) -> list[Match]:
    """Keep the longest match for every byte range; drop overlaps.

    Longest pattern first; among equal lengths, the one with more concrete
    bytes.
    """
    taken: list[tuple[int, int]] = []
    kept: list[Match] = []
    for m in sorted(matches, key=lambda m: (-len(m.sig.pattern), -m.sig.concrete, m.start)):
        if any(m.start < e and s < m.end for s, e in taken):
            continue
        taken.append((m.start, m.end))
        kept.append(m)
    return sorted(kept, key=lambda m: m.start)


def run_level(img: Image, sigs: list[Signature], min_concrete: int,
              stats: dict[str, int]) -> list[Match]:
    """Unique matches of one signature level, merged across SDK versions."""
    by_key: dict[tuple[str, str, int, bytes], Match] = {}
    for s in sigs:
        if s.concrete < min_concrete:
            stats["too_short"] = stats.get("too_short", 0) + 1
            continue
        hits = s.find(img)
        if not hits:
            continue
        if len(hits) > 1:
            stats["ambiguous"] = stats.get("ambiguous", 0) + 1
            continue
        key = (s.lib, s.obj, hits[0], s.pattern)
        if key in by_key:
            by_key[key].versions |= s.versions
        else:
            by_key[key] = Match(s, hits[0], set(s.versions))
    return list(by_key.values())


def match_image(img: Image, public: list[Signature], gen: list[Signature]) -> dict:
    stats: dict[str, int] = {}
    obj_matches = claim(run_level(img, public, MIN_CONCRETE_OBJ, stats))
    claimed = [(m.start, m.end) for m in obj_matches]
    matched_objs = {(m.sig.lib, m.sig.obj) for m in obj_matches}

    # Per-function fallback for objects that did not match whole.
    fn_sigs = [fs for s in public if (s.lib, s.obj) not in matched_objs
               for fs in slice_functions(s)]
    fn_matches = [m for m in claim(run_level(img, fn_sigs, MIN_CONCRETE_FN, stats))
                  if not any(m.start < e and s < m.end for s, e in claimed)]
    claimed += [(m.start, m.end) for m in fn_matches]

    # Short objects (a CTC2 and a return, say) match in many places. Accept
    # one only where it sits flush against an object already placed: the
    # linker packs a library's objects back to back.
    near_matches: list[Match] = []
    placed = {(m.sig.lib, m.sig.obj) for m in obj_matches + fn_matches}
    short = [s for s in public if (s.lib, s.obj) not in placed and MIN_CONCRETE_NEAR <= s.concrete]
    changed = True
    while changed:
        changed = False
        found: dict[int, Match] = {}
        for s in short:
            if (s.lib, s.obj) in placed:
                continue
            flush_hits = []
            for h in s.find(img):
                end = h + len(s.pattern)
                if any(h < e and st < end for st, e in claimed):
                    continue
                if any(0 <= h - e <= NEAR_GAP or 0 <= st - end <= NEAR_GAP for st, e in claimed):
                    flush_hits.append(h)
            # Ambiguous: the object fits flush at more than one site.
            if len(flush_hits) != 1:
                if len(flush_hits) > 1:
                    stats["near_ambiguous"] = stats.get("near_ambiguous", 0) + 1
                continue
            for h in flush_hits:
                prev = found.get(h)
                if prev is None or s.concrete > prev.sig.concrete:
                    found[h] = Match(s, h, set(s.versions))
                elif prev.sig.pattern == s.pattern and prev.sig.obj == s.obj:
                    prev.versions |= s.versions
        for m in claim(list(found.values())):
            if (m.sig.lib, m.sig.obj) in placed or any(m.start < e and st < m.end for st, e in claimed):
                continue
            near_matches.append(m)
            claimed.append((m.start, m.end))
            placed.add((m.sig.lib, m.sig.obj))
            changed = True

    gen_matches = [m for m in claim(run_level(img, gen, MIN_CONCRETE_FN, stats))
                   if not any(m.start < e and s < m.end for s, e in claimed)]

    funcs: dict[int, dict] = {}
    seen_names: dict[str, int] = {}

    def add(pc: int, name: str, m: Match, how: str) -> None:
        if pc in funcs or not C_IDENT.fullmatch(name):
            return
        if name in seen_names and seen_names[name] != pc:
            stats["duplicate_name"] = stats.get("duplicate_name", 0) + 1
            return
        seen_names[name] = pc
        vers = sorted(m.versions, key=lambda v: int(v) if v.isdigit() else 0)
        funcs[pc] = {"pc": pc, "name": name, "lib": m.sig.lib, "obj": m.sig.obj,
                     "how": how, "versions": vers}

    for m in obj_matches:
        for name, off in function_labels(m.sig):
            add(img.base + m.start + off, name, m, "psyq-obj")
    for m in fn_matches:
        add(img.base + m.start, m.sig.labels[0][0], m, "psyq-fn")
    for m in near_matches:
        for name, off in function_labels(m.sig):
            add(img.base + m.start + off, name, m, "psyq-near")
    for m in gen_matches:
        add(img.base + m.start, m.sig.labels[0][0], m, "gen")

    per_lib: dict[str, int] = {}
    for f in funcs.values():
        per_lib[f["lib"]] = per_lib.get(f["lib"], 0) + 1
    version_votes: dict[str, dict[str, int]] = {}
    for m in obj_matches:
        for v in m.versions:
            version_votes.setdefault(m.sig.lib, {}).setdefault(v, 0)
            version_votes[m.sig.lib][v] += 1
    counts = {h: sum(1 for f in funcs.values() if f["how"] == h)
              for h in ("psyq-obj", "psyq-fn", "psyq-near", "gen")}
    return {
        "exe": img.path.name,
        "exe_sha256": img.sha256,
        "load_address": f"0x{img.base:08X}",
        "image_bytes": len(img.data),
        "objects_matched": len(obj_matches),
        "functions_named": len(funcs),
        "by_source": counts,
        "by_lib": dict(sorted(per_lib.items())),
        "lib_versions": {lib: sorted(v, key=lambda x: (-v[x], x))
                         for lib, v in sorted(version_votes.items())},
        "rejected": stats,
        "functions": [dict(f, pc=f"0x{f['pc']:08X}") for f in sorted(funcs.values(), key=lambda f: f["pc"])],
    }


def render_toml(report: dict) -> str:
    lines = [
        f"# PsyQ SDK names for {report['exe']} (sha256 {report['exe_sha256'][:16]}...)",
        "# Generated by psxrecomp tools/psyq_sigs/psyq_sigs.py; merge into the game's symbols.toml.",
        "# status = \"psyq\": a signature match, not a hand check. emit stays false.",
        "",
    ]
    for f in report["functions"]:
        ver = ",".join(f["versions"]) if f["versions"] else "-"
        lines += [
            "[[func]]",
            f"pc = {f['pc']}",
            f"name = \"{f['name']}\"",
            "emit = false",
            "status = \"psyq\"",
            f"note = \"{f['lib']} {f['obj']} ({f['how']}, sdk {ver})\"",
            "",
        ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Our own signatures
# ---------------------------------------------------------------------------

LOAD_STORE = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x32, 0x3A}


def mask_function(code: bytes) -> bytes:
    """Mask the fields that change when the same object is linked elsewhere.

    J/JAL targets and LUI immediates always move. A low immediate (ADDIU,
    ORI, loads, stores) moves when its base register holds a LUI'd high half
    or is $gp/$at, so those are masked too. Branch offsets are relative and
    stay concrete.
    """
    mask = bytearray(b"\xff" * len(code))
    hi_regs: set[int] = set()
    for off in range(0, len(code) - 3, 4):
        w = struct.unpack_from("<I", code, off)[0]
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op in (2, 3):                       # J, JAL: 26-bit target
            mask[off:off + 3] = b"\x00\x00\x00"
            mask[off + 3] &= 0xFC
        elif op == 0x0F:                       # LUI
            mask[off:off + 2] = b"\x00\x00"
            hi_regs.add(rt)
        elif op in (0x09, 0x0D) or op in LOAD_STORE:
            if rs in hi_regs or rs in (1, 28):
                mask[off:off + 2] = b"\x00\x00"
            if op in (0x09, 0x0D) and rt != rs:
                hi_regs.discard(rt)
            elif 0x20 <= op <= 0x26:          # a load overwrites rt
                hi_regs.discard(rt)
    return bytes(mask)


def sig_text(code: bytes, mask: bytes) -> str:
    return " ".join(f"{b:02X}" if m == 0xFF else "??" for b, m in zip(code, mask))


def cmd_gen(args: argparse.Namespace) -> int:
    img = Image.load(args.exe, args.load_address)
    doc = json.loads(args.names.read_text(encoding="utf-8"))
    rows = doc["functions"] if isinstance(doc, dict) else doc
    named = sorted(((int(str(r["pc"]), 0) & 0xFFFFFFFF, r["name"], r.get("lib", ""))
                    for r in rows), key=lambda r: r[0])
    pcs = [r[0] for r in named]
    out = []
    for i, (pc, name, lib) in enumerate(named):
        start = pc - img.base
        if start < 0 or start >= len(img.data):
            continue
        end = pcs[i + 1] - img.base if i + 1 < len(pcs) else len(img.data)
        # Stop at the function's own return (jr ra + delay slot) when that comes first.
        stop = min(end, start + MAX_GEN_BYTES, len(img.data))
        for off in range(start, stop - 3, 4):
            if struct.unpack_from("<I", img.data, off)[0] == 0x03E00008:
                stop = min(stop, off + 8)
                break
        code = img.data[start:stop]
        mask = mask_function(code)
        if sum(1 for m in mask if m) < MIN_CONCRETE_FN:
            continue
        out.append({"name": name, "lib": lib, "sig": sig_text(code, mask)})
    args.out.write_text(json.dumps({
        "format": "psxrecomp-psyq-gen-1",
        "from_exe": img.path.name,
        "from_sha256": img.sha256,
        "signatures": out,
    }, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {args.out}: {len(out)} signatures from {len(named)} named functions")
    return 0


def cmd_match(args: argparse.Namespace) -> int:
    img = Image.load(args.exe, args.load_address)
    public = load_public_set(args.sigs) if args.sigs else []
    gen: list[Signature] = []
    for g in args.gen_sigs or []:
        gen += load_gen_set(g)
    report = match_image(img, public, gen)
    if args.json:
        args.json.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    if args.toml:
        args.toml.write_text(render_toml(report), encoding="utf-8")
    print(f"{report['exe']}: {report['functions_named']} functions named "
          f"({report['by_source']}) in {report['objects_matched']} objects")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("match", help="name SDK functions in an executable")
    m.add_argument("--sigs", type=pathlib.Path, help="public signature set root (version folders)")
    m.add_argument("--gen-sigs", type=pathlib.Path, action="append", help="our own signature file (repeatable)")
    m.add_argument("--exe", type=pathlib.Path, required=True)
    m.add_argument("--load-address", type=lambda s: int(s, 0))
    m.add_argument("--toml", type=pathlib.Path)
    m.add_argument("--json", type=pathlib.Path)
    g = sub.add_parser("gen", help="write our own signatures from a named executable")
    g.add_argument("--exe", type=pathlib.Path, required=True)
    g.add_argument("--load-address", type=lambda s: int(s, 0))
    g.add_argument("--names", type=pathlib.Path, required=True)
    g.add_argument("--out", type=pathlib.Path, required=True)
    args = ap.parse_args(argv)
    return cmd_match(args) if args.cmd == "match" else cmd_gen(args)


if __name__ == "__main__":
    sys.exit(main())
