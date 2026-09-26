#!/usr/bin/env python3
"""gen_testrom.py — emit the cross-executor load-delay test EXE.

Compiled blocks write a load's result into the register file at once and rely
on the recompiler's static load-delay handling for delay-slot readers; the
dirty-RAM interpreter models the load delay value explicitly. This EXE runs the
cases where the two could disagree, on BOTH executors in one boot:

  - the case body runs first from the compiled text image (native);
  - the guest then copies the same position-independent body into RAM outside
    the image (COPY) and runs the copy, which only the dirty-RAM interpreter can
    execute (the runtime has no compiled code for it).

(PSX_FORCE_INTERP cannot be used: it marks the text pages dirty, but the
text-image byte check then finds the compiled image intact and runs it native.)

Cases:
  1a a load in the delay slot of a taken beq; the target reads the register
  1b a load in the delay slot of bgezal (a PC-relative call); the callee's first
     instruction reads it
  2  a syscall in the load's delay slot; the instructions after the handler
     returns read the register
  3  two back-to-back loads to the same register, then reads
  4  a load followed by MTC2 (read back with MFC2), by MTC0 BPC (read back with
     MFC0), and by a store of the register
  5  a load as the last instruction of a block; the next block (a branch
     target) reads it at once

RESULT: native block at 0x800F0000, interpreted block at 0x800F0200, 8 words
per case (word 7 = root-counter-2 delta, informational); done magic at
0x800F0400.
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cycle_testrom"))
from gen_testrom import (REG, R, I, Asm, addiu, addu, beq, bne, jr, li,  # noqa: E402
                         lw, make_psexe, nop, sw)

REG.update({f"$s{i}": 16 + i for i in range(8)})

LOAD_ADDR = 0x80010000
RESULT_NATIVE = 0x800F0000
RESULT_INTERP = 0x800F0200
DONE_ADDR = 0x800F0400
COPY = 0x80100000
DATA = 0x800E0000
DONE_MAGIC = 0xE8EC0D1F
TIMER2 = 0x1F801120
CASES = ["1a_beq_slot_load", "1b_bgezal_slot_load", "2_syscall_in_load_slot",
         "3_back_to_back_loads", "4_mtc2_mtc0_store", "5_load_ends_block"]
V_OLD, V1, V2 = 0x0000A5A5, 0x11112222, 0x33334444


def lhu(rt, off, base): return I(0x25, base, rt, off)
def sh(rt, off, base):  return I(0x29, base, rt, off)
def subu(rd, rs, rt):   return R(0x23, rs, rt, rd)
def jalr(rs):           return R(0x09, rs, 0, 31)
def bgezal(rs, off):    return I(0x01, rs, 0x11, off)
def syscall():          return 0x0000000C
def mtc0(rt, rd):       return (0x10 << 26) | (0x04 << 21) | (REG[rt] << 16) | (rd << 11)
def mfc0(rt, rd):       return (0x10 << 26) | (0x00 << 21) | (REG[rt] << 16) | (rd << 11)
def mtc2(rt, rd):       return (0x12 << 26) | (0x04 << 21) | (REG[rt] << 16) | (rd << 11)
def mfc2(rt, rd):       return (0x12 << 26) | (0x00 << 21) | (REG[rt] << 16) | (rd << 11)


def emit_body(a):
    """The case body: position independent, results at $s7, returns via $s3."""
    a.label("body")
    a.emit(addu("$s3", "$ra", "$zero"))

    def timer_start():
        a.emit(lhu("$s5", 0, "$s6"))

    def timer_stop(base):
        a.emit(lhu("$t9", 0, "$s6"), subu("$t9", "$t9", "$s5"), sw("$t9", base + 0x1C, "$s7"))

    def reset():
        a.emit(li("$t0", V_OLD), li("$t1", 0), li("$t2", 0), li("$t3", 0))

    def save(base, *regs):
        for k, r in enumerate(regs):
            a.emit(sw(r, base + 4 * k, "$s7"))

    # 1a
    reset(); timer_start()
    at = len(a.words)
    a.emit(beq("$zero", "$zero", 0), lw("$t0", 0, "$s4"), nop(), nop())
    tgt = len(a.words)
    a.words[at] = beq("$zero", "$zero", tgt - at - 1)
    a.emit(addu("$t1", "$t0", "$zero"), addu("$t2", "$t0", "$zero"))
    timer_stop(0x00); save(0x00, "$t1", "$t2", "$t0")

    # 1b: bgezal $zero (always taken, links $ra) with the load in its delay slot.
    reset(); timer_start()
    call = len(a.words)
    a.emit(bgezal("$zero", 0), lw("$t0", 0, "$s4"))
    skip = len(a.words)
    a.emit(beq("$zero", "$zero", 0), nop())          # after return: jump over the callee
    callee = len(a.words)
    a.words[call] = bgezal("$zero", callee - call - 1)
    a.emit(addu("$t1", "$t0", "$zero"), addu("$t2", "$t0", "$zero"), jr("$ra"), nop())
    after = len(a.words)
    a.words[skip] = beq("$zero", "$zero", after - skip - 1)
    timer_stop(0x20); save(0x20, "$t1", "$t2", "$t0")

    # 2: EnterCriticalSection syscall in the load's delay slot.
    reset(); a.emit(li("$a0", 1)); timer_start()
    a.emit(lw("$t0", 0, "$s4"), syscall(), addu("$t1", "$t0", "$zero"), addu("$t2", "$t0", "$zero"))
    timer_stop(0x40); save(0x40, "$t1", "$t2", "$t0")
    a.emit(li("$a0", 2), syscall())                  # ExitCriticalSection

    # 3
    reset(); timer_start()
    a.emit(lw("$t0", 0, "$s4"), lw("$t0", 4, "$s4"), addu("$t1", "$t0", "$zero"),
           addu("$t2", "$t0", "$zero"), addu("$t3", "$t0", "$zero"))
    timer_stop(0x60); save(0x60, "$t1", "$t2", "$t3", "$t0")

    # 4
    reset(); timer_start()
    a.emit(lw("$t0", 0, "$s4"), mtc2("$t0", 0), nop(), nop(), mfc2("$t1", 0), nop())
    a.emit(li("$t0", V_OLD), lw("$t0", 0, "$s4"), mtc0("$t0", 3), nop(), mfc0("$t2", 3), nop())
    a.emit(li("$t0", V_OLD), lw("$t0", 0, "$s4"), sw("$t0", 0x80 + 0x14, "$s7"))
    timer_stop(0x80); save(0x80, "$t1", "$t2", "$t0")
    a.emit(mtc0("$zero", 3))

    # 5: the lw ends a block; blk5 is a branch target (of a never-taken bne).
    reset(); timer_start()
    br = len(a.words)
    a.emit(bne("$zero", "$zero", 0), nop(), lw("$t0", 0, "$s4"))
    blk5 = len(a.words)
    a.words[br] = bne("$zero", "$zero", blk5 - br - 1)
    a.emit(addu("$t1", "$t0", "$zero"), addu("$t2", "$t0", "$zero"))
    timer_stop(0xA0); save(0xA0, "$t1", "$t2", "$t0")

    a.emit(jr("$s3"), nop())
    a.label("body_end")


def build():
    a = Asm(LOAD_ADDR)
    a.label("entry")
    a.emit(li("$s6", TIMER2), li("$s4", DATA), li("$t8", DONE_ADDR), sw("$zero", 0, "$t8"))
    for base in (RESULT_NATIVE, RESULT_INTERP):
        a.emit(li("$s7", base))
        for off in range(0, 0xC0, 4):
            a.emit(sw("$zero", off, "$s7"))
    a.emit(li("$t4", V1), sw("$t4", 0, "$s4"), li("$t4", V2), sw("$t4", 4, "$s4"))
    a.emit(sh("$zero", 4, "$s6"))                    # root counter 2: system clock, free-run

    body_fix = len(a.words)
    a.emit(nop(), nop(), nop(), nop(), nop(), nop())  # patched: body address, copy loop args
    # native: call the compiled body
    a.emit(li("$s7", RESULT_NATIVE), jalr("$t5"), nop())
    # copy body -> COPY, one word at a time
    a.emit(li("$t6", COPY), addu("$t7", "$t5", "$zero"))
    loop = len(a.words)
    a.emit(lw("$t4", 0, "$t7"), addiu("$t7", "$t7", 4), sw("$t4", 0, "$t6"), addiu("$t6", "$t6", 4))
    a.emit(bne("$t7", "$s1", 0), nop())
    a.words[len(a.words) - 2] = bne("$t7", "$s1", loop - (len(a.words) - 2) - 1)
    # interpreted: call the copy
    a.emit(li("$s7", RESULT_INTERP), li("$t6", COPY), jalr("$t6"), nop())
    a.emit(li("$t0", DONE_MAGIC), li("$t8", DONE_ADDR), sw("$t0", 0, "$t8"))
    spin = len(a.words)
    a.emit(beq("$zero", "$zero", 0), nop())
    a.words[spin] = beq("$zero", "$zero", -1)

    emit_body(a)
    body, end = a.labels["body"], a.labels["body_end"]
    patch = li("$t5", body) + li("$s1", end)
    while len(patch) < 6:
        patch.append(nop())
    a.words[body_fix:body_fix + 6] = patch
    return a


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "executor_diff_testrom.exe"
    a = build()
    blob = make_psexe(a.words)
    with open(out, "wb") as fh:
        fh.write(blob)
    meta = {
        "load_address": f"0x{LOAD_ADDR:08X}", "text_size": f"0x{len(blob) - 0x800:08X}",
        "result_native": f"0x{RESULT_NATIVE:08X}", "result_interp": f"0x{RESULT_INTERP:08X}",
        "done_addr": f"0x{DONE_ADDR:08X}", "done_magic": f"0x{DONE_MAGIC:08X}",
        "copy": f"0x{COPY:08X}", "body_words": (a.labels["body_end"] - a.labels["body"]) // 4,
        "cases": {name: f"0x{i * 0x20:02X}" for i, name in enumerate(CASES)},
        "values": {"old": f"0x{V_OLD:08X}", "first": f"0x{V1:08X}", "second": f"0x{V2:08X}"},
        "words_per_case": 8, "timer_word": 7,
        "labels": {k: f"0x{v:08X}" for k, v in a.labels.items()},
    }
    with open(out + ".json", "w") as fh:
        json.dump(meta, fh, indent=2)
    with open(os.path.join(os.path.dirname(os.path.abspath(out)), "seeds.txt"), "w",
              newline="\n") as fh:
        for name in ("entry", "body"):
            fh.write(f"0x{a.labels[name]:08X}\n")
    print(f"wrote {out} ({len(blob)} bytes, {len(a.words)} instructions)")


if __name__ == "__main__":
    main()
