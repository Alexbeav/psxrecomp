#!/usr/bin/env python3
"""gen_testrom.py — emit the BIOS resume-PC test EXE (T110).

The August unknown-dispatch family were BIOS ROM resume PCs that no compiled
code can re-enter: mid-block instructions and delay slots, stored as an
exception EPC and dispatched later. This EXE makes exceptions land while the
BIOS kernel is running, on the paths that store and consume those PCs:

  - a root-counter-2 interrupt at a high rate (OpenEvent/EnableEvent/SetRCnt/
    StartRCnt) with a callback, so IRQs are taken inside ROM A0 routines such as
    memcpy, whose loop has the BFC02B7C delay slot from the August list, and
    every take saves and restores EPC through the current TCB;
  - Enter/ExitCriticalSection syscalls in a loop (syscall EPC+4 return).

Everything the harness compares is timing-independent: buffers produced by ROM
routines, the rand() stream and the loop counts. The interrupt counter is timing
and only has to show a sustained load. Both backends run the same EXE from the same synthetic disc, so
the compared RESULT words must match.

RESULT (0x800F0000): +0x00 irq count (timing), +0x10 phase-1 checksum,
+0x14 syscall pairs, +0x18 phase-1 rounds, +0x1C DST fold, +0x20 done magic.
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cycle_testrom"))
from gen_testrom import (REG, R, I, Asm, addiu, addu, bne, beq, li, lui, lw,  # noqa: E402
                         make_psexe, nop, ori, sll, sw)

REG.update({f"$s{i}": 16 + i for i in range(8)})

LOAD_ADDR = 0x80010000
RESULT = 0x800F0000
SRC = 0x80100000
DST = 0x80101000
STR = 0x80100200
DONE_MAGIC = 0x7110D0E0

N_ROM = 1500      # phase 1: ROM A0 routine rounds
N_SYSCALL = 3000  # phase 2: Enter/ExitCriticalSection pairs
RCNT_TARGET = 0x0400


def sb(rt, off, base):  return I(0x28, base, rt, off)
def lbu(rt, off, base): return I(0x24, base, rt, off)
def andi(rt, rs, imm):  return I(0x0C, rs, rt, imm)
def xori(rt, rs, imm):  return I(0x0E, rs, rt, imm)
def subu(rd, rs, rt):   return R(0x23, rs, rt, rd)
def jalr(rs):           return R(0x09, rs, 0, 31)
def jr(rs):             return R(0x08, rs)
def syscall():          return 0x0000000C


def build():
    a = Asm(LOAD_ADDR)
    fix = []  # (word index, "label@reg") for lui/ori address fix-ups

    def bios(vec, fn):
        a.emit(li("$t2", vec), li("$t1", fn), jalr("$t2"), nop())

    def critical(n):
        a.emit(li("$a0", n), syscall())

    def loop_back(top, reg):
        off = (top - (a.here() + 4)) >> 2
        a.emit(bne(reg, "$zero", off), nop())

    a.label("entry")
    a.emit(li("$s7", RESULT))
    for off in range(0, 0x24, 4):
        a.emit(sw("$zero", off, "$s7"))
    a.emit(li("$a0", 1)); bios(0xA0, 0x30)                    # srand(1)

    # source pattern + string
    a.emit(li("$t3", SRC), li("$t0", 0))
    top = a.here()
    a.emit(xori("$t4", "$t0", 0x5A), addu("$t5", "$t3", "$t0"), sb("$t4", 0, "$t5"),
           addiu("$t0", "$t0", 1), xori("$t6", "$t0", 0x100))
    loop_back(top, "$t6")
    a.emit(li("$t3", STR))
    for i, ch in enumerate(b"T110 resume\0"):
        a.emit(ori("$t4", "$zero", ch), sb("$t4", i, "$t3"))

    # interrupt source: root counter 2 event with a callback
    critical(1)
    a.emit(li("$a0", 0xF2000002), li("$a1", 0x0002), li("$a2", 0x1000))
    fix.append((len(a.words), "irq_handler@a3"))
    a.emit(lui("$a3", 0), ori("$a3", "$a3", 0))
    bios(0xB0, 0x08)                                          # OpenEvent
    a.emit(addu("$a0", "$v0", "$zero")); bios(0xB0, 0x0C)     # EnableEvent
    a.emit(li("$a0", 0xF2000002), li("$a1", RCNT_TARGET), li("$a2", 0x1000))
    bios(0xB0, 0x02)                                          # SetRCnt
    a.emit(li("$a0", 0xF2000002)); bios(0xB0, 0x04)           # StartRCnt
    critical(2)

    # phase 1: ROM A0 routines under interrupts
    a.emit(li("$s0", N_ROM), li("$s1", 0), li("$s3", 0))
    top = a.here()
    a.emit(li("$a0", DST), andi("$a1", "$s0", 0xFF), li("$a2", 256)); bios(0xA0, 0x2B)  # memset
    a.emit(li("$a0", DST), li("$a1", SRC), li("$a2", 256)); bios(0xA0, 0x2A)           # memcpy
    a.emit(li("$a0", DST), li("$a1", SRC), li("$a2", 256)); bios(0xA0, 0x2D)           # memcmp
    a.emit(addu("$s1", "$s1", "$v0"))
    bios(0xA0, 0x2F)                                                                    # rand
    a.emit(sll("$t0", "$s1", 5), subu("$s1", "$t0", "$s1"), addu("$s1", "$s1", "$v0"))
    a.emit(li("$a0", STR)); bios(0xA0, 0x1B)                                            # strlen
    a.emit(addu("$s1", "$s1", "$v0"), addiu("$s3", "$s3", 1), addiu("$s0", "$s0", -1))
    loop_back(top, "$s0")
    a.emit(sw("$s1", 0x10, "$s7"), sw("$s3", 0x18, "$s7"))

    # phase 2: syscall pairs
    a.emit(li("$s0", N_SYSCALL), li("$s2", 0))
    top = a.here()
    critical(1)
    a.emit(addiu("$s2", "$s2", 1))
    critical(2)
    a.emit(addiu("$s0", "$s0", -1))
    loop_back(top, "$s0")
    a.emit(sw("$s2", 0x14, "$s7"))

    # fold DST (the last memcpy result)
    a.emit(li("$t3", DST), li("$t0", 0), li("$t7", 0))
    top = a.here()
    a.emit(addu("$t5", "$t3", "$t0"), lbu("$t4", 0, "$t5"), sll("$t6", "$t7", 1),
           addu("$t7", "$t6", "$t4"), addiu("$t0", "$t0", 1), xori("$t6", "$t0", 0x100))
    loop_back(top, "$t6")
    a.emit(sw("$t7", 0x1C, "$s7"))
    a.emit(li("$t0", DONE_MAGIC), sw("$t0", 0x20, "$s7"))
    spin = a.here()
    a.emit(lw("$t0", 0, "$s7"))
    a.emit(beq("$zero", "$zero", (spin - (a.here() + 4)) >> 2), nop())

    # event callback: count interrupts (runs in the kernel's IRQ context)
    a.label("irq_handler")
    # The nop keeps the increment out of the lw's load-delay slot: the R3000A
    # (and Beetle) make the loaded value visible only one instruction later.
    a.emit(li("$t1", RESULT), lw("$t0", 0, "$t1"), nop(), addiu("$t0", "$t0", 1),
           sw("$t0", 0, "$t1"), jr("$ra"), nop())

    for idx, label in fix:
        name, reg = label.split("@")
        addr = a.labels[name]
        a.words[idx] = lui("$" + reg, addr >> 16)
        a.words[idx + 1] = ori("$" + reg, "$" + reg, addr & 0xFFFF)
    return a


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "bios_resume_testrom.exe"
    a = build()
    blob = make_psexe(a.words)
    with open(out, "wb") as fh:
        fh.write(blob)
    meta = {
        "load_address": f"0x{LOAD_ADDR:08X}", "text_size": f"0x{len(blob) - 0x800:08X}",
        "result": f"0x{RESULT:08X}", "done_magic": f"0x{DONE_MAGIC:08X}",
        "compare_offsets": ["0x10", "0x14", "0x18", "0x1C"],
        "expect": {"0x14": N_SYSCALL, "0x18": N_ROM},
        "labels": {k: f"0x{v:08X}" for k, v in a.labels.items()},
    }
    with open(out + ".json", "w") as fh:
        json.dump(meta, fh, indent=2)
    # The event callback is reached only through a pointer handed to the BIOS,
    # so the recompiler needs it as a seed.
    with open(os.path.join(os.path.dirname(os.path.abspath(out)), "seeds.txt"), "w",
              newline="\n") as fh:
        for name in ("entry", "irq_handler"):
            fh.write(f"0x{a.labels[name]:08X}\n")
    print(f"wrote {out} ({len(blob)} bytes, {len(a.words)} instructions)")


if __name__ == "__main__":
    main()
