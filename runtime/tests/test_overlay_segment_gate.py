#!/usr/bin/env python3
"""Build the real overlay loader and prove shards run only for their segment.

A shard bakes the segment it was compiled for into every PC it produces:
jal/jalr link values, jump and exception resume PCs, I-cache fetch tags and
store-PC stamps. OpenBIOS's exception path enters its RAM patch slots at KUSEG
(0x0000281C), and a KSEG0 shard run there wrote $ra=0x8000282C instead of
0x0000282C and missed I-cache lines the kernel had just filled with KUSEG tags:
R4 warm (native) and cold (interpreted) overlay-cache runs took the next VBlank
IRQ at different instructions and split at guest frame 194 (#417).

docs/SEGMENT_AWARE_CODE.md §5.7 (rollout PR E) keys shards by segment: capture
records each dispatch's segment, compile_overlays.py builds one shard per
segment with entries, KUSEG and KSEG1 shards live in the seg-kuseg/ and
seg-kseg1/ subdirectories of the cache tag (KSEG0 keeps its old place and
names), and a manifest's `S <segment>` record names the segment its full-VA F
entries are in. This drives the loader harness (overlay_pair_dedup_harness.c)
through the real overlay_loader.c:

  segment-alias   a KSEG0 shard at 0x80010000 runs only for its KSEG0 PC; the
                  KUSEG and KSEG1 aliases fall to the interpreter (#417's case).
  kuseg-only      a KUSEG-compiled shard runs natively for its KUSEG PCs, entry
                  and CPS continuation, and the KSEG0 and KSEG1 aliases of the
                  same bytes are interpreted (the §7.1 acceptance case).
  all-segments    one shard per segment for the same bytes: each PC runs its
                  own segment's shard, loaded lazily from its directory, and
                  none pair-aliases another; PCs in segments that map no RAM
                  never run one.
  rejected        a manifest whose S record, or whose F entries, disagree with
                  its directory's segment is never indexed, loaded or
                  published.
  stale           a PC whose own segment's shard no longer matches live bytes
                  is interpreted, not handed to another segment's shard.
  continuation    a CPS continuation resumes its own segment's owner, even
                  where another segment has a function entry at that word,
                  and loads it lazily even when another segment's bundle is
                  newer.
  capture wiring  the interpreter records each dispatch's segment from its
                  full PC, and every clear of dispatch evidence clears the
                  segment bits too (a source check).

Usage: python runtime/tests/test_overlay_segment_gate.py
Exit 0 = PASS.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import re
import shutil
import sys
import tempfile
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import test_overlay_pair_dedup_runtime as pair  # noqa: E402

KUSEG, KSEG0, KSEG1 = 0x00000000, 0x80000000, 0xA0000000
SEG_DIR = {KUSEG: "seg-kuseg", KSEG0: None, KSEG1: "seg-kseg1"}
INSTANCE = {KSEG0: 1, KUSEG: 3, KSEG1: 4}
SEG_DEFINE = {KSEG0: None, KUSEG: "TEST_SEGMENT_KUSEG", KSEG1: "TEST_SEGMENT_KSEG1"}


def segment_manifest(seg: int, *, s_record: int | None, f_seg: int | None = None,
                     length: int = 8, phys: int = 0x00010000,
                     stale: bool = False) -> str:
    """One function at `phys` covering `length` bytes. R stays a KSEG0-spelled
    byte extent whatever the segment. `stale` records a CRC the live (zeroed)
    RAM does not have."""
    entry = (seg if f_seg is None else f_seg) | phys
    crc = zlib.crc32(bytes(length) if not stale else b"\xFF" * length) & 0xFFFFFFFF
    lines = []
    if s_record is not None:
        lines.append(f"S {s_record:08X}\n")
    lines.append(f"P {pair.PAIR:016X}\n")
    lines.append(f"F {entry:08X} {crc:08X}\n")
    lines.append(f"R {KSEG0 | phys:08X} {length:08X}\n")
    return "".join(lines)


def compile_segment_fixture(gcc: str, out: pathlib.Path, seg: int) -> None:
    command = [gcc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", "-shared",
               f"-I{pair.RUNTIME / 'include'}",
               f"-DTEST_INSTANCE={INSTANCE[seg]}"]
    if SEG_DEFINE[seg]:
        command.append(f"-D{SEG_DEFINE[seg]}=1")
    if platform.system() != "Windows":
        command.append("-fPIC")
    command += [str(pair.TESTS / "overlay_pair_dedup_fixture.c"), "-o", str(out)]
    pair.run(command)


def publish(cache: pathlib.Path, subdir: str | None, library: pathlib.Path,
            text: str, *, resident: bool) -> pathlib.Path:
    ext = ".dll" if platform.system() == "Windows" else ".so"
    leaf = cache / pair.GAME / "gcc" / pair.arch_abi() / pair.codegen_leaf()
    if subdir:
        leaf = leaf / subdir
    leaf.mkdir(parents=True, exist_ok=True)
    # The same {phys}_{crc} name in every segment's directory (decision 4).
    target = leaf / f"00010000_11111111{ext}"
    shutil.copy2(library, target)
    target.with_suffix(".ranges").write_text(text, encoding="ascii", newline="")
    if resident:
        target.with_suffix(".resident").write_text(
            pair.RESIDENT, encoding="ascii", newline="")
    return target


def check_capture_wiring() -> None:
    """Capture records the segment of each interpreted dispatch from its full
    PC, and no site drops dispatch evidence without dropping the segment bits
    with it (a stale segment would name a segment the next epoch never ran)."""
    src = (pair.RUNTIME / "src")
    interp = (src / "dirty_ram_interp.c").read_text(encoding="utf-8")
    start = interp.index("static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, "
                         "uint32_t stop_addr) {")
    body = interp[start:interp.index("\n}\n", start)]
    any_bit = body.find("g_dirty_ram_dispatch_pc_bitmap[word >> 5] |=")
    seg_bit = body.find("int seg = psx_code_segment_index(addr);")
    seg_set = body.find("g_dirty_ram_dispatch_seg_bitmap[seg][word >> 5] |=")
    if not (0 <= any_bit < seg_bit < seg_set):
        raise AssertionError("dirty_ram_dispatch_inner must set the segment bit "
                             "from the full PC next to the dispatch bit")
    for name in ("memory.c", "overlay_capture.c", "dirty_ram_interp.c"):
        text = (src / name).read_text(encoding="utf-8")
        if re.search(r"memset\(\s*&?g_dirty_ram_dispatch_pc_bitmap", text):
            raise AssertionError(f"{name} clears dispatch evidence without its "
                                 "segment bits (use dirty_ram_dispatch_evidence_clear)")


def main() -> int:
    check_capture_wiring()
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default=shutil.which("gcc") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.gcc:
        raise SystemExit("gcc/cc is required")
    ext = ".dll" if platform.system() == "Windows" else ".so"
    exe = ".exe" if platform.system() == "Windows" else ""
    with tempfile.TemporaryDirectory(
            prefix="psx-segment-gate-", ignore_cleanup_errors=True) as raw:
        tmp = pathlib.Path(raw)
        harness = tmp / f"segment-harness{exe}"
        pair.compile_harness(args.gcc, harness)
        os.environ.pop("PSX_PAIR_TEST_TRACE", None)

        # #417: a KSEG0 shard, and no other segment's.
        shard = tmp / f"shard{ext}"
        pair.compile_fixture(args.gcc, shard, instance=1)
        cache = tmp / "cache-alias"
        first = pair.publish(cache, "gcc", f"00010000_11111111{ext}", shard,
                             pair.manifest([(0x80010000, 4), (0x80010004, 4)]))
        pair.run([str(harness), str(cache), "segment-alias", str(first),
                  str(tmp / f"unused{ext}")])

        libs = {}
        for seg in (KSEG0, KUSEG, KSEG1):
            libs[seg] = tmp / f"seg-{seg:08X}{ext}"
            compile_segment_fixture(args.gcc, libs[seg], seg)

        # §7.1: a KUSEG shard alone, loaded lazily from seg-kuseg/.
        cache = tmp / "cache-kuseg"
        publish(cache, SEG_DIR[KUSEG], libs[KUSEG],
                segment_manifest(KUSEG, s_record=KUSEG), resident=False)
        pair.run([str(harness), str(cache), "segment-shards", "kuseg", "-"])

        # All three segments for the same bytes: KSEG0 preloaded (resident),
        # KUSEG and KSEG1 found lazily in their own directories. The KSEG0
        # manifest has no S record, as every manifest before §5.7.
        cache = tmp / "cache-all"
        publish(cache, None, libs[KSEG0],
                segment_manifest(KSEG0, s_record=None), resident=True)
        publish(cache, SEG_DIR[KUSEG], libs[KUSEG],
                segment_manifest(KUSEG, s_record=KUSEG), resident=False)
        publish(cache, SEG_DIR[KSEG1], libs[KSEG1],
                segment_manifest(KSEG1, s_record=KSEG1), resident=True)
        pair.run([str(harness), str(cache), "segment-shards",
                  "kseg0,kuseg,kseg1", "-"])

        # Disagreeing segments are never loaded, resident or lazy.
        rejected = {
            # A KUSEG shard whose manifest has no S record (so KSEG0).
            "kuseg-no-s": (SEG_DIR[KUSEG], libs[KUSEG],
                           segment_manifest(KUSEG, s_record=None)),
            # S names KSEG0 but the directory is seg-kuseg/.
            "kuseg-s-kseg0": (SEG_DIR[KUSEG], libs[KSEG0],
                              segment_manifest(KSEG0, s_record=KSEG0)),
            # A KUSEG manifest in the KSEG0 place.
            "root-s-kuseg": (None, libs[KUSEG],
                             segment_manifest(KUSEG, s_record=KUSEG)),
            # S says KSEG1, an F entry is KSEG0.
            "kseg1-f-kseg0": (SEG_DIR[KSEG1], libs[KSEG0],
                              segment_manifest(KSEG1, s_record=KSEG1, f_seg=KSEG0)),
            # S after an F record (the F is right for the S, not before it),
            # and S twice.
            "s-after-f": (None, libs[KSEG0],
                          segment_manifest(KSEG0, s_record=None) + "S 80000000\n"),
            "s-twice": (SEG_DIR[KUSEG], libs[KUSEG],
                        "S 00000000\n" + segment_manifest(KUSEG, s_record=KUSEG)),
            # S must name a segment base that maps RAM.
            "s-kseg2": (SEG_DIR[KUSEG], libs[KUSEG],
                        segment_manifest(KUSEG, s_record=0xC0000000)),
            "s-not-base": (SEG_DIR[KUSEG], libs[KUSEG],
                           segment_manifest(KUSEG, s_record=0x00010000)),
        }
        for name, (subdir, lib, text) in rejected.items():
            for resident in (False, True):
                cache = tmp / f"cache-{name}-{int(resident)}"
                target = publish(cache, subdir, lib, text, resident=resident)
                # Nothing indexes or loads it, and live publication refuses it.
                pair.run([str(harness), str(cache), "segment-shards", "none",
                          str(target)])

        # The KUSEG shard's bytes are stale, the KSEG0 shard's are live.
        cache = tmp / "cache-stale"
        publish(cache, None, libs[KSEG0],
                segment_manifest(KSEG0, s_record=None), resident=True)
        publish(cache, SEG_DIR[KUSEG], libs[KUSEG],
                segment_manifest(KUSEG, s_record=KUSEG, stale=True), resident=True)
        pair.run([str(harness), str(cache), "segment-stale", "-", "-"])

        # 0x10004: a KSEG0 function entry, and a KUSEG CPS continuation.
        cache = tmp / "cache-continuation"
        publish(cache, None, libs[KSEG0],
                segment_manifest(KSEG0, s_record=None, phys=0x00010004, length=4),
                resident=True)
        publish(cache, SEG_DIR[KUSEG], libs[KUSEG],
                segment_manifest(KUSEG, s_record=KUSEG), resident=True)
        pair.run([str(harness), str(cache), "segment-continuation", "-", "-"])

        # Both unloaded, the KSEG0 bundle newer: the KUSEG continuation still
        # loads the KUSEG shard.
        cache = tmp / "cache-lazy-continuation"
        k0 = publish(cache, None, libs[KSEG0],
                     segment_manifest(KSEG0, s_record=None), resident=False)
        publish(cache, SEG_DIR[KUSEG], libs[KUSEG],
                segment_manifest(KUSEG, s_record=KUSEG), resident=False)
        newer = os.stat(k0).st_mtime + 60
        for path in (k0, k0.with_suffix(".ranges")):
            os.utime(path, (newer, newer))
        pair.run([str(harness), str(cache), "segment-continuation", "lazy", "-"])
    print("PASS: overlay shards run only for the segment they were compiled for")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
