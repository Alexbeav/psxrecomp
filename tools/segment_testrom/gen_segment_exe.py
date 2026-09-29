#!/usr/bin/env python3
"""gen_segment_exe.py - a synthetic KUSEG-linked PS-X EXE for segment-aware code.

docs/SEGMENT_AWARE_CODE.md is the design this program tests.

The header carries KUSEG addresses (load and entry 0x00010000), as Alien
Resurrection (SLUS-00633) and Kula World (SCES-01000) do. On hardware, and in
Beetle, the whole program then runs with KUSEG PCs. Every `jal` inherits the
segment of the PC that executes it (Beetle cpu.c DO_BRANCH, mask 0xF0000000).
So the link values it writes, the EPCs it produces and the I-cache tags it
fetches with are all KUSEG.

The program exercises each place the segment reaches:

  R+0x00  $ra seen by `leaf` after `jal`                    (link, KUSEG)
  R+0x04  $ra seen by `leaf` after `bgezal`                 (link, KUSEG)
  R+0x08  $ra seen by `seeded` after `jalr`                 (link, KUSEG)
  R+0x10  T2 ticks around `probe_run` at its KUSEG home     (cached fetch)
  R+0x14  `getpc` link inside that run                      (0x0001xxxx)
  R+0x18  T2 ticks around `probe_run` via its KSEG0 alias   (cached fetch)
  R+0x1C  `getpc` link inside that run                      (0x8001xxxx)
  R+0x20  T2 ticks around `probe_run` via its KSEG1 alias   (uncached, +4/fetch)
  R+0x24  `getpc` link inside that run                      (0xA001xxxx)

`seeded` is entered through a register. The test seeds it in the link
segment and checks that the recompiler accepts the seed. `probe_run` has a straight run of ten instructions after
its `getpc` call returns, which is enough to show per-instruction uncached
fetch charging. The results block R (0x00011000) lies outside the loaded
image, so the stores never touch code pages. Timer 2 is put in mode 0
(system clock, free-running) first. The T2 deltas include the call overhead,
which is the same for all three runs. Each delta is (after - before) & 0xFFFF,
because T2 is a 16-bit counter that wraps. The subtraction waits one `nop`
after the second `lw`: it would otherwise sit in the load delay slot and read
the stale register.

The deltas compare like with like. Each of the three call sequences starts
on an I-cache line, so the main-loop fetches between the two T2 reads cost
the same in every run. `probe_run` starts its own line, so no earlier code
(`seeded`) leaves one of its lines filled. Beetle tags lines with the full
virtual address, so every run starts with the probe body cold. The KSEG0 and
KUSEG deltas are then equal, and the KSEG1 delta exceeds them by the uncached
fetch surcharge alone. The program ends in a `j .` spin, where an oracle run
can read R.

The MIPS is hand-encoded, so the output needs no toolchain and is
reproducible byte for byte. recompiler/tests/test_segment_aware_codegen.py
imports build() and probes() from this file.

Usage:  python gen_segment_exe.py [out.exe]   (also writes out.probes.json)
"""

import json
import os
import struct
import sys

KUSEG, KSEG0, KSEG1 = 0x00000000, 0x80000000, 0xA0000000
SEGMENTS = {"kuseg": KUSEG, "kseg0": KSEG0, "kseg1": KSEG1}

LOAD = 0x00010000          # link segment: KUSEG
RESULTS = 0x00011000       # outside the image; never a code page
IO_BASE = 0x1F800000       # T2 count at +0x1120, mode at +0x1124
STACK_TOP = 0x001FFF00     # KUSEG, like the rest of the header

REG = {"zero": 0, "at": 1, "v0": 2, "v1": 3, "a0": 4, "t0": 8, "t1": 9,
       "t2": 10, "t3": 11, "s0": 16, "s1": 17, "t9": 25, "sp": 29, "ra": 31}


def _r(x):
    return REG[x] if isinstance(x, str) else x


def R(funct, rs=0, rt=0, rd=0, sa=0):
    return (_r(rs) << 21) | (_r(rt) << 16) | (_r(rd) << 11) | (sa << 6) | funct


def I(op, rs=0, rt=0, imm=0):
    return (op << 26) | (_r(rs) << 21) | (_r(rt) << 16) | (imm & 0xFFFF)


def nop():              return 0
def addu(rd, rs, rt):   return R(0x21, rs, rt, rd)
def subu(rd, rs, rt):   return R(0x23, rs, rt, rd)
def or_(rd, rs, rt):    return R(0x25, rs, rt, rd)
def jr(rs):             return R(0x08, rs)
def jalr(rs, rd="ra"):  return R(0x09, rs, 0, rd)
def addiu(rt, rs, imm): return I(0x09, rs, rt, imm)
def andi(rt, rs, imm):  return I(0x0C, rs, rt, imm)
def ori(rt, rs, imm):   return I(0x0D, rs, rt, imm)
def lui(rt, imm):       return I(0x0F, 0, rt, imm)
def lw(rt, off, base):  return I(0x23, base, rt, off)
def sw(rt, off, base):  return I(0x2B, base, rt, off)


class Asm:
    """Two-pass assembler: fixups resolve labels once layout is known."""

    def __init__(self, base):
        self.base, self.words, self.labels, self.fixups = base, [], {}, []

    def here(self):
        return self.base + 4 * len(self.words)

    def label(self, name):
        self.labels[name] = self.here()

    def emit(self, *words):
        self.words.extend(words)

    def line_up(self, before=0):
        """Pad with nops until here() + before starts a 16-byte I-cache line."""
        while (self.here() + before) & 0xF:
            self.words.append(nop())

    def jal(self, name):
        self.fixups.append((len(self.words), "jal", name))
        self.words.append(0)

    def j(self, name):
        self.fixups.append((len(self.words), "j", name))
        self.words.append(0)

    def bgezal(self, rs, name):
        self.fixups.append((len(self.words), "bgezal", (rs, name)))
        self.words.append(0)

    def la(self, rt, name, segment):
        """lui/ori of label `name` in `segment` (the probe's alias)."""
        self.fixups.append((len(self.words), "la_hi", (rt, name, segment)))
        self.fixups.append((len(self.words) + 1, "la_lo", (rt, name, segment)))
        self.words.extend([0, 0])

    def resolve(self):
        for i, kind, arg in self.fixups:
            pc = self.base + 4 * i
            if kind in ("jal", "j"):
                target = self.labels[arg]
                # J-type keeps PC[31:28]: a target in another segment is not
                # encodable, which is the invariant the design relies on.
                assert (target & 0xF0000000) == ((pc + 4) & 0xF0000000)
                op = 0x03 if kind == "jal" else 0x02
                self.words[i] = (op << 26) | ((target >> 2) & 0x03FFFFFF)
            elif kind == "bgezal":
                rs, name = arg
                off = (self.labels[name] - (pc + 4)) >> 2
                self.words[i] = I(0x01, rs, 0x11, off)
            else:
                rt, name, segment = arg
                va = segment | (self.labels[name] & 0x1FFFFFFF)
                self.words[i] = (lui(rt, va >> 16) if kind == "la_hi"
                                 else ori(rt, rt, va & 0xFFFF))
        return b"".join(struct.pack("<I", w) for w in self.words)


def _assemble():
    a = Asm(LOAD)
    a.label("main")
    a.emit(lui("s0", RESULTS >> 16), ori("s0", "s0", RESULTS & 0xFFFF))
    a.emit(lui("s1", IO_BASE >> 16))
    a.emit(sw("zero", 0x1124, "s1"))            # T2 mode 0: sysclock, free run
    a.jal("leaf"); a.emit(nop(), sw("v0", 0x00, "s0"))
    a.bgezal("zero", "leaf"); a.emit(nop(), sw("v0", 0x04, "s0"))
    a.la("t0", "seeded", KUSEG)
    a.emit(jalr("t0"), nop(), sw("v0", 0x08, "s0"))
    for n, segment in enumerate((KUSEG, KSEG0, KSEG1)):
        a.line_up(8)                            # each `lw t1` starts a line
        a.la("t0", "probe_run", segment)
        # T2 before and after the call. The nop keeps subu out of the second
        # lw's load delay slot; T2 is 16 bits, so mask the difference.
        a.emit(lw("t1", 0x1120, "s1"), jalr("t0"), nop(),
               lw("t2", 0x1120, "s1"), nop(), subu("t2", "t2", "t1"),
               andi("t2", "t2", 0xFFFF),
               sw("t2", 0x10 + 8 * n, "s0"), sw("v1", 0x14 + 8 * n, "s0"))
    a.label("spin")
    a.j("spin"); a.emit(nop())

    a.label("leaf")
    a.emit(jr("ra"), addu("v0", "ra", "zero"))
    a.label("seeded")                           # entered through a register
    a.emit(jr("ra"), or_("v0", "ra", "zero"))

    a.line_up()                                 # no line shared with `seeded`
    a.label("probe_run")
    a.emit(addu("t9", "ra", "zero"))
    a.jal("getpc"); a.emit(nop())
    a.label("probe_run_straight")               # leader: getpc returns here
    a.emit(*[addiu("t3", "t3", 1) for _ in range(8)])
    a.emit(jr("t9"), addu("v0", "t9", "zero"))
    a.label("probe_run_end")

    a.label("getpc")
    a.emit(jr("ra"), addu("v1", "ra", "zero"))
    a.label("end")
    return a


def build():
    """Return the EXE bytes (2048-byte header + image padded to 2 KiB)."""
    a = _assemble()
    body = a.resolve()
    body += b"\0" * (-len(body) % 0x800)
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, a.labels["main"])  # initial pc (KUSEG)
    struct.pack_into("<I", h, 0x18, LOAD)              # load address (KUSEG)
    struct.pack_into("<I", h, 0x1C, len(body))
    struct.pack_into("<I", h, 0x30, STACK_TOP)         # stack base (KUSEG)
    return bytes(h) + body


def probes():
    """Label addresses (link segment) and probe facts the test checks."""
    a = _assemble()
    a.resolve()
    labels = dict(a.labels)
    return {
        "link_segment": KUSEG,
        "load": LOAD,
        "results": RESULTS,
        "labels": labels,
        # Seeds, written the way a project writes them: `seeded` in the link
        # segment, and `probe_run` (whose direct callee `getpc` follows it)
        # in the two alias segments it is entered through.
        "seeds": {
            "home": [labels["seeded"]],
            "variants": [KSEG0 | labels["probe_run"], KSEG1 | labels["probe_run"]],
        },
        # Straight-line fetch run inside probe_run: runtime PCs, link segment.
        "straight": list(range(labels["probe_run_straight"],
                               labels["probe_run_end"], 4)),
    }


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "segment_testrom.exe"
    with open(out, "wb") as f:
        f.write(build())
    info = probes()
    with open(os.path.splitext(out)[0] + ".probes.json", "w") as f:
        json.dump({k: ({n: "0x%08X" % v for n, v in info[k].items()}
                       if k == "labels" else info[k]) for k in info},
                  f, indent=2)
    print("wrote %s (%d bytes), probe_run at 0x%08X" %
          (out, len(build()), info["labels"]["probe_run"]))


if __name__ == "__main__":
    main()
