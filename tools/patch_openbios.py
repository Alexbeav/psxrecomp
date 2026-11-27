#!/usr/bin/env python3
"""The local patch carried by bios/openbios.bin, and the tool that applies and checks it.

bios/openbios.bin is PCSX-Redux OpenBIOS built at the commits pinned in
bios/OpenBIOS.toml (the upstream image, sha256 UPSTREAM_SHA256 below), with
the words in PATCH replaced. The patch is written from the documented
contract of the BIOS call, A(34h) free(buf), and from a disassembly of this
image; no other implementation was read.

Why: the kernel's free (multi_free, RAM 0x21B0, reached through A(34h) and
the kernel's own free) keeps an address-ordered free list that ends at the
sentinel 0x6538, and walks it until it finds where the block goes. The walk
had no guard, so a list whose link does not lie above its node never ends.
Wipeout XL hands InitHeap a region it then manages itself, and its control
block overwrites the BIOS block header at the heap start; its later
free(0x80195F10) then looped forever on 0x800FE100 -> 0x1610 -> code words
-> 0 -> 0 (PS1G-142). The retail BIOS returns from the same call.

The patch keeps every step of the walk on a valid list and adds one check:
when the walk is about to move on to the next node, that node must lie above
the current one. If it does not, free returns and leaves the block out of
the list (the memory is not reused; nothing is written). The check fits in
place: the two branch-delay nops before the walk compute the comparison, and
the walk's tail keeps the previous block's size in t2 instead of a3, so
nothing else in the image moves.

Usage:
  python tools/patch_openbios.py --check [IMAGE]       image == upstream + PATCH
  python tools/patch_openbios.py --apply UPSTREAM OUT  write upstream + PATCH
  python tools/patch_openbios.py --revert IMAGE OUT    write the upstream image back
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

UPSTREAM_SHA256 = "fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57"
PATCHED_SHA256 = "32dbc89c2e620c9a671fabe05772c3ae0ab38e4d15b4fdeeb6f7ca07093ef8c3"

# Kernel ramtext: ROM 0xBFC1E4D4.. is copied to RAM 0x500 at boot (bios/OpenBIOS.toml).
RAMTEXT_ROM = 0x1E4D4
RAMTEXT_RAM = 0x500
RAMTEXT_LEN = 0x46A0

REG = {n: i for i, n in enumerate(
    "zr at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}


def _r(fn: int, rd: str, rs: str, rt: str) -> int:
    return (REG[rs] << 21) | (REG[rt] << 16) | (REG[rd] << 11) | fn


def _branch(op: int, rs: str, rt: str, pc: int, target: int) -> int:
    off = (target - (pc + 4)) >> 2
    assert -0x8000 <= off < 0x8000
    return (op << 26) | (REG[rs] << 21) | (REG[rt] << 16) | (off & 0xFFFF)


def _mem(op: int, rt: str, off: int, base: str) -> int:
    return (op << 26) | (REG[base] << 21) | (REG[rt] << 16) | (off & 0xFFFF)


def sltu(pc, rd, rs, rt): return _r(0x2B, rd, rs, rt)
def addu(pc, rd, rs, rt): return _r(0x21, rd, rs, rt)
def or_(pc, rd, rs, rt): return _r(0x25, rd, rs, rt)
def jr(pc, rs): return (REG[rs] << 21) | 0x08
def beq(pc, rs, rt, target): return _branch(0x04, rs, rt, pc, target)
def bne(pc, rs, rt, target): return _branch(0x05, rs, rt, pc, target)
def lw(pc, rt, off, base): return _mem(0x23, rt, off, base)
def sw(pc, rt, off, base): return _mem(0x2B, rt, off, base)
def nop(pc): return 0


# (RAM address, upstream word, assembly). Registers inside multi_free:
# a0 = buf, v1 = header (buf - 8), a2 = its size, a1 = previous node (the
# list flag before the walk), v0 = current node, a3 = sentinel 0x6538,
# t0 = (header < node). Branch targets: 0x2254 compares the header with the
# node, 0x22C8 is `jr ra`, 0x2300 links a block that does not touch the
# previous one, 0x22C4 stores prev->next.
_ASM = [
    (0x2210, 0x00000000, sltu, ("t1", "a1", "v0")),     # delay of the first step: flag < head
    (0x2258, 0x00000000, sltu, ("t1", "a1", "v0")),     # delay of "move on": prev < next
    (0x22D0, 0x00402825, beq, ("t1", "zr", 0x22C8)),    # not above the previous node: return
    (0x22D4, 0x8C420000, or_, ("a1", "v0", "zr")),      # prev = node
    (0x22D8, 0x00000000, lw, ("v0", 0, "a1")),          # node = prev->next
    (0x22DC, 0x1447FFDD, lw, ("t2", 4, "a1")),          # prev->size (fills the load delay)
    (0x22E0, 0x0062402B, bne, ("v0", "a3", 0x2254)),    # not the sentinel: compare
    (0x22E4, 0x8CA70004, sltu, ("t0", "v1", "v0")),     # delay: header < node
    (0x22E8, 0x00000000, addu, ("t0", "a1", "t2")),     # end of the last block
    (0x22EC, 0x00A74021, bne, ("v1", "t0", 0x2300)),    # does not touch it: link at the end
    (0x22F0, 0x14680003, addu, ("t2", "t2", "a2")),     # delay: merged size
    (0x22F4, 0x00E63821, jr, ("ra",)),
    (0x22F8, 0x03E00008, sw, ("t2", 4, "a1")),          # delay: prev->size += size
    (0x22FC, 0xACA70004, nop, ()),
]

PATCH = [(ram, old, fn(ram, *args)) for ram, old, fn, args in _ASM]


def rom_offset(ram: int) -> int:
    assert RAMTEXT_RAM <= ram < RAMTEXT_RAM + RAMTEXT_LEN
    return RAMTEXT_ROM + (ram - RAMTEXT_RAM)


def _word(img: bytes, ram: int) -> int:
    return struct.unpack_from("<I", img, rom_offset(ram))[0]


def apply(upstream: bytes) -> bytes:
    if hashlib.sha256(upstream).hexdigest() != UPSTREAM_SHA256:
        raise ValueError("not the pinned upstream OpenBIOS image")
    out = bytearray(upstream)
    for ram, old, new in PATCH:
        if _word(out, ram) != old:
            raise ValueError("0x%04X holds 0x%08X, expected 0x%08X" % (ram, _word(out, ram), old))
        struct.pack_into("<I", out, rom_offset(ram), new)
    return bytes(out)


def revert(patched: bytes) -> bytes:
    out = bytearray(patched)
    for ram, old, new in PATCH:
        if _word(out, ram) != new:
            raise ValueError("0x%04X holds 0x%08X, expected the patched 0x%08X" % (ram, _word(out, ram), new))
        struct.pack_into("<I", out, rom_offset(ram), old)
    if hashlib.sha256(out).hexdigest() != UPSTREAM_SHA256:
        raise ValueError("reverting the patch does not give the pinned upstream image")
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", nargs="?", const=str(Path(__file__).resolve().parents[1] / "bios" / "openbios.bin"))
    g.add_argument("--apply", nargs=2, metavar=("UPSTREAM", "OUT"))
    g.add_argument("--revert", nargs=2, metavar=("IMAGE", "OUT"))
    a = ap.parse_args()
    if a.check:
        img = Path(a.check).read_bytes()
        sha = hashlib.sha256(img).hexdigest()
        up = revert(img)
        if apply(up) != img:
            print("FAIL: image differs from upstream outside the patch")
            return 1
        print("ok: %s = upstream %s + %d patched words" % (sha, UPSTREAM_SHA256[:12], len(PATCH)))
        if sha != PATCHED_SHA256:
            print("FAIL: sha256 %s, expected %s" % (sha, PATCHED_SHA256))
            return 1
        return 0
    src, dst = (a.apply or a.revert)
    data = Path(src).read_bytes()
    out = apply(data) if a.apply else revert(data)
    Path(dst).write_bytes(out)
    print("wrote %s sha256 %s" % (dst, hashlib.sha256(out).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
