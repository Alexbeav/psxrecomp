"""Collect dirty_ram dispatch misses in the game text range and append
new unique addresses to a persistent seeds file. Run this during gameplay
to build up the seed list over time.

A seed is a PC, so it carries a segment (docs/SEGMENT_AWARE_CODE.md §5.3,
§5.4). dirty_ram_stats keys its rows by physical address, so the tool writes
each one in the title's link segment: the segment of its EXE's load address
(KSEG0 0x8001xxxx for most titles, KUSEG 0x0001xxxx for Kula World or Alien
Resurrection). The recompiler takes a seed in any other segment as a variant
request, not an entry: it compiles the home body's direct-edge closure again
in that segment (§5.4). Segment misses (TCP `segment_misses`: static text
entered in a segment with no body of its own) are written with their full PC,
so the next regeneration compiles a variant for each.

The link segment comes from --segment, from --game-toml ([game].load_address,
or the header of its [game].exe), or from game_text_lo when that is written as
a KSEG0/KSEG1 address. A physical game_text_lo alone cannot tell KUSEG from
KSEG0, so the tool refuses to guess.

Usage: python3 collect_game_misses.py <port> <game_text_lo> <game_text_hi> <output_file>
                                      [--segment kuseg|kseg0|kseg1] [--game-toml PATH]
Example (Tomba): python3 collect_game_misses.py 4470 0x80010000 0x80098000 seeds/dirty_ram_misses.txt
Example (KUSEG): python3 collect_game_misses.py 4720 0x10000 0x98000 seeds/misses.txt --game-toml game.toml
"""
import argparse
import json
import os
import socket
import struct
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        tomllib = None

PHYS_MASK = 0x1FFFFFFF
SEG_MASK = 0xE0000000
SEGMENTS = {"kuseg": 0x00000000, "kseg0": 0x80000000, "kseg1": 0xA0000000}


def maps_physical(pc):
    """KUSEG below 0x20000000, KSEG0 and KSEG1 map physical memory; a PC
    anywhere else addresses no RAM, so it names no code of the image, and the
    recompiler refuses it as a seed."""
    return pc < 0x20000000 or (pc & SEG_MASK) in (0x80000000, 0xA0000000)


def parse_segment(text):
    key = text.strip().lower()
    if key in SEGMENTS:
        return SEGMENTS[key]
    value = int(key, 0)
    if (value & SEG_MASK) not in SEGMENTS.values():
        raise ValueError("not a RAM segment: %s" % text)
    return value & SEG_MASK


def segment_from_game_toml(path):
    if tomllib is None:
        raise SystemExit("--game-toml needs Python 3.11+ (tomllib) or tomli; pass --segment")
    with open(path, "rb") as f:
        game = tomllib.load(f).get("game", {})
    if "load_address" in game:
        return int(str(game["load_address"]), 0) & SEG_MASK, "%s [game].load_address" % path
    exe = game.get("exe") or game.get("rom")
    if not exe:
        raise SystemExit("%s: [game] has neither load_address nor exe" % path)
    exe_path = exe if os.path.isabs(exe) else os.path.join(os.path.dirname(os.path.abspath(path)), exe)
    with open(exe_path, "rb") as f:
        header = f.read(0x20)
    if len(header) < 0x20 or header[:8] != b"PS-X EXE":
        raise SystemExit("%s: not a PS-X EXE (from %s [game].exe)" % (exe_path, path))
    return struct.unpack_from("<I", header, 0x18)[0] & SEG_MASK, "%s header" % exe_path


def send(port, cmd, **kw):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(json.dumps({"id": 1, "cmd": cmd, **kw}).encode() + b"\n")
    d = b""
    while True:
        c = s.recv(65536)
        if not c:
            break
        d += c
        try:
            json.loads(d.decode())
            break
        except ValueError:
            pass
    s.close()
    return json.loads(d.decode())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", nargs="?", default="4470")
    ap.add_argument("lo", nargs="?", default="0x10000", help="game text start (inclusive)")
    ap.add_argument("hi", nargs="?", default="0x98000", help="game text end (exclusive)")
    ap.add_argument("out_file", nargs="?", default="dirty_ram_misses.txt")
    ap.add_argument("--segment", help="link segment: kuseg, kseg0, kseg1 or an address in it")
    ap.add_argument("--game-toml", help="read the link segment from this game.toml")
    a = ap.parse_args(argv)
    port, lo, hi = int(a.port, 0), int(a.lo, 0), int(a.hi, 0)

    if a.segment:
        seg, source = parse_segment(a.segment), "--segment"
    elif a.game_toml:
        seg, source = segment_from_game_toml(a.game_toml)
    elif lo & SEG_MASK:
        seg, source = lo & SEG_MASK, "game_text_lo"
    else:
        raise SystemExit("cannot tell the link segment from a physical game_text_lo "
                         "(0x%X): pass --game-toml game.toml or --segment "
                         "kuseg|kseg0|kseg1" % lo)
    if seg not in SEGMENTS.values():
        raise SystemExit("link segment 0x%08X (%s) is not KUSEG, KSEG0 or KSEG1" % (seg, source))
    print("link segment 0x%08X (%s)" % (seg, source))
    lo_phys, hi_phys = lo & PHYS_MASK, hi & PHYS_MASK

    # Load existing known addresses (exact PCs: a seed's segment matters).
    known = set()
    if os.path.exists(a.out_file):
        with open(a.out_file) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    try:
                        known.add(int(line.split()[0], 0))
                    except ValueError:
                        pass

    r = send(port, "dirty_ram_stats")
    if not r.get("ok"):
        print("error:", r)
        return 1

    new_entries = []
    for entry in r.get("per_pc", []):
        phys = int(entry["pc"], 16) & PHYS_MASK
        if lo_phys <= phys < hi_phys:
            vaddr = seg | phys
            if vaddr not in known:
                new_entries.append((vaddr, entry["hits"],
                                    "dirty_ram miss: %d hits, %d insns" % (entry["hits"], entry["insns"])))
                known.add(vaddr)

    # Segment misses name their full PC: a variant request (§5.4).
    sm = send(port, "segment_misses")
    if sm.get("ok"):
        for row in sm.get("summary", []):
            vaddr = int(row["pc"], 16)
            if not maps_physical(vaddr):
                print("warning: segment miss 0x%08X is in a segment that does not map "
                      "physical memory; not a seed (the recompiler refuses it)" % vaddr)
                continue
            if lo_phys <= (vaddr & PHYS_MASK) < hi_phys and vaddr not in known:
                new_entries.append((vaddr, row["count"],
                                    "segment miss: %d hits, home %s (variant request)"
                                    % (row["count"], row["home"])))
                known.add(vaddr)
    else:
        print("note: no segment_misses command on this runtime (%s)" % sm.get("error", sm))

    if new_entries:
        new_entries.sort(key=lambda x: -x[1])
        with open(a.out_file, "a") as f:
            for vaddr, _, note in new_entries:
                f.write("0x%08X  # %s\n" % (vaddr, note))
        print("Appended %d new addresses to %s" % (len(new_entries), a.out_file))
        for vaddr, _, note in new_entries:
            print("  0x%08X  (%s)" % (vaddr, note))
    else:
        print("No new addresses (already have %d in %s)" % (len(known), a.out_file))
    return 0


if __name__ == "__main__":
    sys.exit(main())
