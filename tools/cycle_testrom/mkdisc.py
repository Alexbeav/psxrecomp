#!/usr/bin/env python3
"""mkdisc.py - build a minimal PS1 boot disc (MODE2/2352 BIN + CUE) in pure Python.

A stand-in for mkpsxiso where it is not available (macOS/Linux without the
Windows binary). Writes an ISO9660 volume with SYSTEM.CNF and one boot EXE,
as Mode 2 Form 1 sectors with a correct EDC/ECC (Beetle verifies them).

Sectors 0-15 (the license area) are zero-filled unless --license-from names
a raw MODE2/2352 .bin of a PS1 disc you own; its first 16 sectors are copied
verbatim. That area is copyrighted Sony data: keep the built image local
(disc/*.bin and disc/*.cue are gitignored), never commit or redistribute it.
It is needed even with OpenBIOS: Beetle's CD controller reports a disc
without the license string in sector 4 as unlicensed, and OpenBIOS then
retries GetID forever instead of booting.

Usage (from tools/cycle_testrom, after gen_testrom.py):
  python3 mkdisc.py --cnf disc/SYSTEM.CNF --exe cycle_testrom.exe \
      --exe-name CYCT_001.01 --volume CYCT00101 --out disc/cyctest.bin \
      --license-from "<a PS1 disc you own>.bin"
"""
import argparse, os, struct

SECTOR = 2352
USER = 2048

# ---- EDC / ECC (ECMA-130 Annex A/B; same tables as ecm.c / Mednafen lec) ----
EDC_TABLE = []
for i in range(256):
    e = i
    for _ in range(8):
        e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    EDC_TABLE.append(e)

def edc(data):
    e = 0
    for b in data:
        e = (e >> 8) ^ EDC_TABLE[(e ^ b) & 0xFF]
    return e

ECC_F = [0] * 256
ECC_B = [0] * 256
for i in range(256):
    j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
    ECC_F[i] = j
    ECC_B[i ^ j] = i

def ecc_block(sec, major_count, minor_count, major_mult, minor_inc, dest):
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = sec[0xC + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = ECC_F[a]
        a = ECC_B[ECC_F[a] ^ b]
        sec[dest + major] = a
        sec[dest + major + major_count] = a ^ b

def bcd(n):
    return ((n // 10) << 4) | (n % 10)

def mode2_form1(lba, user, submode=0x08):
    assert len(user) == USER
    s = bytearray(SECTOR)
    s[0] = 0x00
    s[1:11] = b"\xFF" * 10
    s[11] = 0x00
    a = lba + 150
    s[12:16] = bytes([bcd(a // 4500), bcd((a // 75) % 60), bcd(a % 75), 2])
    sub = bytes([0, 0, submode, 0])
    s[16:20] = sub
    s[20:24] = sub
    s[24:24 + USER] = user
    s[2072:2076] = struct.pack("<I", edc(s[16:2072]))
    hdr = bytes(s[12:16])
    s[12:16] = b"\0\0\0\0"          # mode 2: header excluded from ECC
    ecc_block(s, 86, 24, 2, 86, 0x81C)   # P
    ecc_block(s, 52, 43, 86, 88, 0x8C8)  # Q
    s[12:16] = hdr
    return bytes(s)

# ---- ISO9660 ----
def both16(v): return struct.pack("<H", v) + struct.pack(">H", v)
def both32(v): return struct.pack("<I", v) + struct.pack(">I", v)
def padstr(s, n): return s.encode("ascii").ljust(n, b" ")[:n]
DATE7 = bytes([126, 9, 29, 0, 0, 0, 0])   # 2026-09-29 00:00:00 GMT
DATE17 = b"2026092900000000" + b"\0"

def dirrec(ident, lba, size, is_dir):
    ln = 33 + len(ident)
    if ln & 1:
        ln += 1
    r = bytearray(ln)
    r[0] = ln
    r[2:10] = both32(lba)
    r[10:18] = both32(size)
    r[18:25] = DATE7
    r[25] = 2 if is_dir else 0
    r[28:32] = both16(1)
    r[32] = len(ident)
    r[33:33 + len(ident)] = ident
    return bytes(r)

def sectors_for(n):
    return max(1, (n + USER - 1) // USER)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cnf", required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--exe-name", required=True, help="ISO name, e.g. CYCT_001.01")
    p.add_argument("--volume", default="PSXDISC")
    p.add_argument("--out", required=True, help="output .bin (a .cue is written beside it)")
    p.add_argument("--license-from", help="raw MODE2/2352 .bin of a PS1 disc you own: "
                   "copy its sectors 0-15 (the license area) verbatim")
    p.add_argument("--pad-sectors", type=int, default=300)
    a = p.parse_args()

    cnf = open(a.cnf, "rb").read()
    exe = open(a.exe, "rb").read()
    files = sorted([(a.exe_name.upper() + ";1", exe), ("SYSTEM.CNF;1", cnf)])

    LBA_PVD, LBA_TERM, LBA_LPT, LBA_MPT, LBA_ROOT = 16, 17, 18, 19, 20
    lba = 21
    layout = []
    for name, data in files:
        layout.append((name, lba, data))
        lba += sectors_for(len(data))
    total = max(lba, a.pad_sectors)

    root = bytearray()
    root += dirrec(b"\x00", LBA_ROOT, USER, True)
    root += dirrec(b"\x01", LBA_ROOT, USER, True)
    for name, flba, data in layout:
        root += dirrec(name.encode("ascii"), flba, len(data), False)
    assert len(root) <= USER
    root = bytes(root).ljust(USER, b"\0")

    pt_l = struct.pack("<BBIH", 1, 0, LBA_ROOT, 1) + b"\0\0"
    pt_m = struct.pack(">BBIH", 1, 0, LBA_ROOT, 1) + b"\0\0"

    pvd = bytearray(USER)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = padstr("PLAYSTATION", 32)
    pvd[40:72] = padstr(a.volume, 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(USER)
    pvd[132:140] = both32(len(pt_l))
    pvd[140:144] = struct.pack("<I", LBA_LPT)
    pvd[148:152] = struct.pack(">I", LBA_MPT)
    pvd[156:190] = dirrec(b"\x00", LBA_ROOT, USER, True)
    pvd[190:318] = padstr(a.volume, 128)
    pvd[318:446] = padstr("PSXRECOMP", 128)
    pvd[446:574] = padstr("MKDISC.PY", 128)
    pvd[574:702] = padstr("PLAYSTATION", 128)
    pvd[702:739] = padstr("", 37)
    pvd[739:776] = padstr("", 37)
    pvd[776:813] = padstr("", 37)
    for off in (813, 830, 847, 864):
        pvd[off:off + 17] = DATE17
    pvd[881] = 1

    term = bytearray(USER)
    term[0] = 0xFF
    term[1:6] = b"CD001"
    term[6] = 1

    out = bytearray()
    if a.license_from:
        with open(a.license_from, "rb") as f:
            lic = f.read(16 * SECTOR)
        assert len(lic) == 16 * SECTOR
        out += lic
    else:
        for i in range(16):
            out += mode2_form1(i, bytes(USER))
    out += mode2_form1(LBA_PVD, bytes(pvd), 0x09)
    out += mode2_form1(LBA_TERM, bytes(term), 0x89)
    out += mode2_form1(LBA_LPT, pt_l.ljust(USER, b"\0"), 0x89)
    out += mode2_form1(LBA_MPT, pt_m.ljust(USER, b"\0"), 0x89)
    out += mode2_form1(LBA_ROOT, root, 0x89)
    cur = 21
    for name, flba, data in layout:
        n = sectors_for(len(data))
        for k in range(n):
            chunk = data[k * USER:(k + 1) * USER].ljust(USER, b"\0")
            out += mode2_form1(cur, chunk, 0x89 if k == n - 1 else 0x08)
            cur += 1
    while cur < total:
        out += mode2_form1(cur, bytes(USER))
        cur += 1

    with open(a.out, "wb") as f:
        f.write(out)
    cue = os.path.splitext(a.out)[0] + ".cue"
    with open(cue, "w") as f:
        f.write('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                % os.path.basename(a.out))
    print(f"wrote {a.out} ({total} sectors) and {cue}")

if __name__ == "__main__":
    main()
