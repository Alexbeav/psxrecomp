#!/usr/bin/env python3
"""OpenBIOS free must return on a free list whose link does not lie above its node (PS1G-142).

Runs the kernel's free (multi_free, RAM 0x21B0) from the bytes of
bios/openbios.bin in a small R3000 interpreter: the kernel ramtext is copied
to RAM 0x500 as the boot code does, the call enters through the A(34h)
user_free wrapper (0x2588) or the kernel's kern_free (0x25D0), and the run
stops when it returns to a marker address or after a step limit.

  1. The Wipeout XL shape: user list head 0x800FE100, whose link is 0x1610
     (kernel code); free(0x80195F10). The checked-in image returns and writes
     nothing. The upstream image, rebuilt by reverting tools/patch_openbios.py,
     runs out of steps: that is the hang the patch removes.
  2. Valid lists, both lists, 400 seeded cases (empty list, block before the
     head, between nodes, after the tail, touching either neighbour): the
     checked-in image and the upstream image return and make the same stores
     in the same order, leaving identical RAM outside the kernel text and
     identical v0/v1.
  3. The image is the upstream image plus the patch words and nothing else,
     at the sha256 bios/OpenBIOS.toml pins.

The interpreter refuses an instruction that reads the register loaded by the
instruction before it (the R3000 load delay), so a patch that relies on
either reading of that slot fails here.
"""

from __future__ import annotations

import argparse
import hashlib
import random
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import patch_openbios as P  # noqa: E402

RAM_SIZE = 0x200000
RETURN_MARK = 0x00000040  # an address free never reaches on its own
USER_FREE, KERN_FREE = 0x2588, 0x25D0
SENTINEL, USER_HEAD, KERN_HEAD = 0x6538, 0x6544, 0x6540
STEP_LIMIT = 20000


class Hang(Exception):
    pass


class Machine:
    def __init__(self, image: bytes):
        self.ram = bytearray(RAM_SIZE)
        self.ram[P.RAMTEXT_RAM:P.RAMTEXT_RAM + P.RAMTEXT_LEN] = image[P.RAMTEXT_ROM:P.RAMTEXT_ROM + P.RAMTEXT_LEN]
        self.r = [0] * 32
        self.writes = []

    def rd(self, addr: int) -> int:
        phys = addr & 0x1FFFFFFF
        if addr & 3:
            raise AssertionError("unaligned load 0x%08X" % addr)
        if phys < RAM_SIZE * 4:  # main RAM and its mirrors
            return struct.unpack_from("<I", self.ram, phys & (RAM_SIZE - 1))[0]
        return 0  # open bus: nothing a valid list reaches

    def wr(self, addr: int, val: int) -> None:
        phys = addr & 0x1FFFFFFF
        if addr & 3 or phys >= RAM_SIZE * 4:
            raise AssertionError("store to 0x%08X" % addr)
        self.writes.append((phys & (RAM_SIZE - 1), val))
        struct.pack_into("<I", self.ram, phys & (RAM_SIZE - 1), val & 0xFFFFFFFF)

    def call(self, entry: int, a0: int) -> int:
        self.r[4], self.r[31], self.r[29] = a0, RETURN_MARK, 0x801FFF00
        pc, npc, loaded, steps = entry, entry + 4, None, 0
        while pc != RETURN_MARK:
            steps += 1
            if steps > STEP_LIMIT:
                raise Hang("no return after %d instructions, pc 0x%04X" % (STEP_LIMIT, pc))
            w = self.rd(pc)
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            reads = {rs, rt} if op in (0, 4, 5, 0x2B) else {rs}
            if loaded is not None and loaded != 0 and loaded in reads and w != 0:
                raise AssertionError("pc 0x%04X reads $%d in the load delay slot" % (pc, loaded))
            loaded = None
            R, nxt = self.r, npc + 4
            if op == 0:
                fn = w & 0x3F
                if fn == 0x08:
                    nxt = R[rs]
                elif fn == 0x00:
                    R[rd] = (R[rt] << ((w >> 6) & 31)) & 0xFFFFFFFF
                elif fn == 0x21:
                    R[rd] = (R[rs] + R[rt]) & 0xFFFFFFFF
                elif fn == 0x23:
                    R[rd] = (R[rs] - R[rt]) & 0xFFFFFFFF
                elif fn == 0x24:
                    R[rd] = R[rs] & R[rt]
                elif fn == 0x25:
                    R[rd] = R[rs] | R[rt]
                elif fn == 0x2B:
                    R[rd] = int(R[rs] < R[rt])
                else:
                    raise AssertionError("special 0x%02X at 0x%04X" % (fn, pc))
            elif op == 2:
                nxt = (npc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            elif op == 3:
                R[31] = npc + 4
                nxt = (npc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            elif op in (4, 5):
                if (R[rs] == R[rt]) == (op == 4):
                    nxt = (npc + (simm << 2)) & 0xFFFFFFFF
            elif op == 9:
                R[rt] = (R[rs] + simm) & 0xFFFFFFFF
            elif op == 0x0F:
                R[rt] = imm << 16
            elif op == 0x23:
                R[rt] = self.rd((R[rs] + simm) & 0xFFFFFFFF)
                loaded = rt
            elif op == 0x2B:
                self.wr((R[rs] + simm) & 0xFFFFFFFF, R[rt])
            else:
                raise AssertionError("opcode 0x%02X at 0x%04X" % (op, pc))
            R[0] = 0
            pc, npc = npc, nxt
        return steps


def w32(m: Machine, addr: int, val: int) -> None:
    struct.pack_into("<I", m.ram, addr & (RAM_SIZE - 1), val)


def upstream_and_checked_in(path: Path) -> tuple[bytes, bytes]:
    img = path.read_bytes()
    if hashlib.sha256(img).hexdigest() == P.UPSTREAM_SHA256:
        return img, img  # an unpatched image: test 1 shows the hang
    return P.revert(img), img


def wipeout_shape(image: bytes) -> tuple[Machine, bytes]:
    m = Machine(image)
    w32(m, USER_HEAD, 0x800FE100)
    w32(m, KERN_HEAD, SENTINEL)
    w32(m, 0xFE0FC, 0x00018EF0)  # the game's control block over the heap start
    w32(m, 0xFE100, 0x00001610)
    w32(m, 0xFE104, 0x00001F20)
    w32(m, 0x195F08, 0x8018FEF0)  # header of the block being freed
    w32(m, 0x195F0C, 0x00004000)
    return m, bytes(m.ram)


def test_wipeout_shape(up: bytes, fixed: bytes) -> None:
    m, before = wipeout_shape(fixed)
    steps = m.call(USER_FREE, 0x80195F10)
    assert bytes(m.ram) == before, "the guarded free wrote %r" % m.writes[:4]
    print("image under test: free(0x80195F10) on head 0x800FE100 -> 0x1610 returned in %d instructions, wrote nothing" % steps)
    m, _ = wipeout_shape(up)
    try:
        m.call(USER_FREE, 0x80195F10)
    except Hang as e:
        print("upstream image: %s (the PS1G-142 hang)" % e)
    else:
        raise AssertionError("the upstream image returned: this test no longer shows the fault")


def build_list(m: Machine, rng: random.Random, head_var: int) -> list[tuple[int, int]]:
    """An address-ordered free list of 0..5 blocks in 0x80100000..0x80180000, 8-byte aligned."""
    blocks, addr = [], 0x80100000 + rng.randrange(0, 0x400, 8)
    for _ in range(rng.randrange(0, 6)):
        size = rng.choice([0x10, 0x18, 0x40, 0x100, 0x800])
        blocks.append((addr, size))
        addr += size + rng.choice([0, 0x10, 0x20, 0x200, 0x1000])  # 0: touching neighbours
    for i, (a, s) in enumerate(blocks):
        w32(m, a, blocks[i + 1][0] if i + 1 < len(blocks) else SENTINEL)
        w32(m, a + 4, s)
    w32(m, head_var, blocks[0][0] if blocks else SENTINEL)
    return blocks


def pick_block(rng: random.Random, blocks: list[tuple[int, int]]) -> tuple[int, int]:
    size = rng.choice([0x10, 0x20, 0x80])
    if not blocks or rng.random() < 0.2:
        return (0x80100000 - size - rng.choice([0, 0x10, 0x100]), size)  # before the head
    i = rng.randrange(len(blocks))
    a, s = blocks[i]
    end = a + s
    nxt = blocks[i + 1][0] if i + 1 < len(blocks) else 0x80200000
    room = nxt - end
    if room < size:
        return (blocks[0][0] - size, size)  # touching the head from below
    choices = [end, nxt - size]  # touching the previous, touching the next
    if room >= size + 0x10:
        choices.append(end + ((room - size) // 2 & ~7))
    return (rng.choice(choices), size)


def test_valid_lists(up: bytes, fixed: bytes) -> None:
    rng = random.Random(142)
    kinds = {}
    for case in range(400):
        entry, head_var, other = rng.choice([(USER_FREE, USER_HEAD, KERN_HEAD), (KERN_FREE, KERN_HEAD, USER_HEAD)])
        seed = rng.random()
        results = []
        for image in (up, fixed):
            r2 = random.Random(seed)
            m = Machine(image)
            w32(m, other, SENTINEL)
            blocks = build_list(m, r2, head_var)
            hdr, size = pick_block(r2, blocks)
            w32(m, hdr, 0xDEADBEEF)
            w32(m, hdr + 4, size)
            m.call(entry, hdr + 8)
            lo, hi = P.RAMTEXT_RAM, P.RAMTEXT_RAM + P.RAMTEXT_LEN  # the kernel copy differs by the patch itself
            results.append((bytes(m.ram[:lo]) + bytes(m.ram[hi:]), m.writes, m.r[2], m.r[3]))
        assert results[0] == results[1], "case %d: the patched free differs from upstream" % case
        kinds[len(blocks)] = kinds.get(len(blocks), 0) + 1
    print("valid lists: 400 cases, identical stores (in order), RAM outside the kernel text, v0 and v1; list lengths %s"
          % dict(sorted(kinds.items())))


def test_image_pin(up: bytes, fixed: bytes) -> None:
    assert hashlib.sha256(up).hexdigest() == P.UPSTREAM_SHA256
    assert P.apply(up) == fixed
    diff = [i for i in range(0, len(up), 4) if up[i:i + 4] != fixed[i:i + 4]]
    assert len(diff) == len(P.PATCH) - sum(1 for _, o, n in P.PATCH if o == n), diff
    toml = (ROOT / "bios" / "OpenBIOS.toml").read_text(encoding="utf-8")
    pinned = re.search(r'^sha256\s*=\s*"([0-9a-f]{64})"', toml, re.M).group(1)
    sha = hashlib.sha256(fixed).hexdigest()
    assert pinned == sha == P.PATCHED_SHA256, (pinned, sha, P.PATCHED_SHA256)
    print("image: upstream %s + %d words = %s, pinned in bios/OpenBIOS.toml" % (P.UPSTREAM_SHA256[:12], len(diff), sha[:12]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", type=Path, default=ROOT / "bios" / "openbios.bin",
                    help="image under test (default: the checked-in bios/openbios.bin)")
    a = ap.parse_args()
    try:
        up, fixed = upstream_and_checked_in(a.image)
        test_wipeout_shape(up, fixed)
        test_image_pin(up, fixed)
        test_valid_lists(up, fixed)
    except (Hang, AssertionError, ValueError) as e:
        print("FAIL: %s" % e)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
