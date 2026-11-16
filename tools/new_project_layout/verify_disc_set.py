#!/usr/bin/env python3
"""Check that N probed discs form one coherent multi-disc set, and say which kind.

Used by tools/new_project_layout/setup_project.{sh,ps1} when --disc is passed
more than once. Consumes probe_disc.py JSON dumps in disc order (disc 1 first)
and answers one question: can this framework build the set today?

Two kinds of multi-disc title (see docs/MULTI_DISC.md "Two axes, not one"):

  data-only   every disc boots the same program; the later discs are just more
              data. One recompiled program covers the set. This includes a
              program patched per disc: equal header fields and size, with a
              few pages of the loaded image differing (a disc number, a file
              name). The whole-file hash differs for such a set; the per-page
              comparison below is what identifies it.
  N-programs  each disc carries its own boot executable, so the set needs N
              statically recompiled programs selected by mounted serial.

The framework can scaffold the first kind today. It cannot build the second:
the game emitter still emits unprefixed func_XXXXXXXX / k_psx_game_dispatch
(recompiler/src/main_psx.cpp), so two programs collide at link. That is P2 of
docs/MULTI_DISC.md. This script refuses an N-programs set by name rather than
quietly scaffolding a one-program project that covers only disc 1.

Exit codes:
  0  set is coherent and data-only (or a single disc) — safe to scaffold
  2  usage error
  3  set needs N programs — not buildable yet (docs/MULTI_DISC.md P1+P2)
  4  set is incoherent (duplicate disc, mixed release, unreadable probe)
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Fields that give the *shape* of the program a disc boots. If any of these
# differs between discs, the discs boot different programs.
#
# `serial` and `boot_exe` are deliberately NOT here. They name the DISC, not
# the program: Final Fantasy VII ships ONE byte-identical executable on three
# discs as SCUS_941.63/.64/.65 under serials SCUS-94163/64/65, so comparing
# those strings reported three programs where there is one, and refused a set
# the framework can already build.
PROGRAM_SHAPE_FIELDS = (
    "entry_pc",
    "load_address",
    "text_size",
    "stack_base",
    "boot_exe_size",
)

# Equal shape is necessary, not sufficient. The content decides:
#
#   * equal `boot_exe_sha256`: the same file, one program;
#   * different hash, and the loaded images differ in a few 4 KiB pages only:
#     one program patched per disc. Measured: Metal Gear Solid (Europe) differs
#     in one byte of a path string, Star Wars: Rebel Assault II in one
#     instruction's immediate (the disc number), Dragon Warrior VII in two
#     blocks of padding. The whole-file hash alone called each of those sets
#     "N programs" and refused it;
#   * different hash, and many pages differ: another program that happens to
#     share the header fields.
#
# The bound is deliberately small. A per-disc patch touches a constant or a
# name; a different program differs across most of its image. A set above the
# bound is refused as before.
PATCHED_PAGE_LIMIT = 8
PATCHED_PAGE_DIVISOR = 16
PAGE_BYTES = 4096

# Per-disc identity. Recorded in disc_set.json and shown in the summary, but
# never used to decide how many programs a set needs.
DISC_IDENTITY_FIELDS = ("serial", "boot_exe")

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_NEEDS_N_PROGRAMS = 3
EXIT_INCOHERENT = 4


def load_probe(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as e:
        raise SystemExit(f"cannot read probe JSON {path}: {e}")
    except json.JSONDecodeError as e:
        raise SystemExit(f"probe JSON {path} is not valid JSON: {e}")
    if not isinstance(data, dict):
        raise SystemExit(f"probe JSON {path} is not an object")
    return data


def content_key(p: dict) -> tuple:
    """What makes two probes the same *image*.

    The TOC fingerprint alone is not enough: it is derived from the track
    layout, so two different discs of one release that happen to share a
    layout produce the same fp. Requiring the volume id and data-track size to
    agree as well keeps a legitimate sibling disc from being called a
    duplicate, while still catching the same image passed twice.
    """
    return (
        str(p.get("required_disc_fp") or ""),
        str(p.get("data_track_sha1") or ""),
        str(p.get("volume_id") or ""),
        p.get("data_track_size"),
    )


def serial_prefix(serial: str) -> str:
    """SCUS-94163 -> SCUS. The publisher/region family of the release."""
    s = str(serial or "").strip().upper()
    return s[:4] if len(s) >= 4 else s


def patched_page_limit(page_count: int) -> int:
    """How many differing pages one per-disc patched program may have."""
    return min(PATCHED_PAGE_LIMIT, max(1, page_count // PATCHED_PAGE_DIVISOR))


def page_crcs(p: dict) -> list[str] | None:
    """The probe's per-page CRC list, or None when the probe has none."""
    crcs = p.get("boot_exe_page_crc32")
    if isinstance(crcs, list) and crcs and all(isinstance(c, str) and c for c in crcs):
        return crcs
    return None


def page_address(p: dict, index: int) -> str:
    """RAM address of loaded-image page `index`; page 0 starts at the load address."""
    try:
        load = int(str(p.get("load_address") or ""), 16)
    except ValueError:
        return f"page {index}"
    if index == 0:
        return f"0x{load:08X}"
    return f"0x{(load - load % PAGE_BYTES) + index * PAGE_BYTES:08X}"


def describe(index: int, p: dict) -> str:
    return (
        f"  disc {index}: {p.get('serial') or '<no serial>'}"
        f"  boot={p.get('boot_exe') or '?'}"
        f"  entry={p.get('entry_pc') or '?'}"
        f"  tracks={p.get('track_count', '?')}"
        f"  {p.get('cue_name') or ''}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Verify N probed discs form one buildable set."
    )
    ap.add_argument(
        "probe_json",
        nargs="+",
        help="probe_disc.py --json-out dumps, in disc order (disc 1 first)",
    )
    ap.add_argument(
        "--json-out",
        default="",
        help="write the verdict as JSON (disc_set.json)",
    )
    args = ap.parse_args()

    paths = [Path(a).expanduser() for a in args.probe_json]
    probes = [load_probe(p) for p in paths]
    count = len(probes)

    print(f"== Disc set ({count} disc{'s' if count != 1 else ''}) ==")
    for i, p in enumerate(probes, start=1):
        print(describe(i, p))

    problems: list[str] = []
    warnings: list[str] = []

    # --- every disc must have identified a program -------------------------
    for i, p in enumerate(probes, start=1):
        if not str(p.get("serial") or "").strip():
            problems.append(f"disc {i} has no serial — probe could not read SYSTEM.CNF")
        if not str(p.get("entry_pc") or "").strip():
            problems.append(f"disc {i} has no entry_pc — probe could not read the PS-X EXE")

    # --- the same disc passed twice ----------------------------------------
    seen_path: dict[str, int] = {}
    seen_content: dict[tuple, int] = {}
    for i, p in enumerate(probes, start=1):
        cue = str(p.get("cue_path") or "")
        if cue and cue in seen_path:
            problems.append(
                f"disc {i} is the same file as disc {seen_path[cue]} ({cue}) — "
                "pass each disc once"
            )
        elif cue:
            seen_path[cue] = i
        key = content_key(p)
        if key in seen_content:
            problems.append(
                f"disc {i} ({p.get('cue_name')}) is the same image as disc "
                f"{seen_content[key]} — same fingerprint, volume id and size"
            )
        else:
            seen_content[key] = i

    # A shared TOC fingerprint across discs is legal but defeats the runtime's
    # disc identity check, which is what [netplay] required_disc_fp gates on.
    fps = [str(p.get("required_disc_fp") or "") for p in probes]
    if len(probes) > 1 and len({f for f in fps if f}) == 1 and fps[0]:
        warnings.append(
            "every disc has the same TOC fingerprint — [netplay] "
            "required_disc_fp cannot tell these discs apart"
        )

    # --- mixed release (US disc 1 + PAL disc 2, etc.) ----------------------
    prefixes = {serial_prefix(p.get("serial", "")) for p in probes}
    prefixes.discard("")
    if len(prefixes) > 1:
        listed = ", ".join(
            f"disc {i} {serial_prefix(p.get('serial', '')) or '?'}"
            for i, p in enumerate(probes, start=1)
        )
        problems.append(
            f"discs are from different releases ({listed}) — a set must be one "
            "release; check you have not mixed regions"
        )

    if problems:
        print()
        sys.stdout.flush()
        print("error: this is not a coherent disc set:", file=sys.stderr)
        for m in problems:
            print(f"  - {m}", file=sys.stderr)
        return EXIT_INCOHERENT

    # --- data-only vs N-programs -------------------------------------------
    first = probes[0]

    # A missing hash must not read as "these agree". Empty compares equal to
    # empty, so a probe that failed to extract the executable would silently
    # turn N programs into one -- the exact silent-wrong-answer this gate is
    # here to prevent. Refuse to judge instead.
    missing = [i for i, p in enumerate(probes, start=1)
               if not str(p.get("boot_exe_sha256") or "").strip()]
    if missing:
        print()
        sys.stdout.flush()
        print("error: cannot tell how many programs this set needs.", file=sys.stderr)
        print(f"  disc(s) {', '.join(str(i) for i in missing)} have no "
              f"boot_exe_sha256 — the probe could not read the boot executable.",
              file=sys.stderr)
        print("  Re-probe with a current probe_disc.py; refusing to guess.",
              file=sys.stderr)
        return EXIT_INCOHERENT

    differing: list[str] = []
    for field in PROGRAM_SHAPE_FIELDS:
        values = {str(p.get(field) or "") for p in probes}
        # A probe written before `boot_exe_size` existed has no value for it.
        # An absent value is not evidence either way: compare the sizes only
        # when every probe carries one. The header fields are always present.
        if field == "boot_exe_size" and "" in values:
            continue
        if len(values) > 1:
            differing.append(field)

    # Content, once the shape agrees. `patched` lists, for each later disc
    # whose executable is not disc 1's file, the loaded-image pages that differ.
    patched: list[dict] = []
    if not differing:
        base = page_crcs(first)
        for i, p in enumerate(probes[1:], start=2):
            if str(p.get("boot_exe_sha256")) == str(first.get("boot_exe_sha256")):
                continue
            crcs = page_crcs(p)
            if base is None or crcs is None or len(base) != len(crcs):
                # Nothing to tell a patched copy from another program. Refuse,
                # as the whole-file comparison always did.
                differing.append(
                    f"boot_exe_sha256 (disc {i}: no page fingerprints to "
                    "compare; re-probe with a current probe_disc.py)"
                )
                continue
            pages = [n for n, (a, b) in enumerate(zip(base, crcs)) if a != b]
            limit = patched_page_limit(len(base))
            if len(pages) > limit:
                differing.append(
                    f"boot_exe_sha256 (disc {i}: {len(pages)} of {len(base)} "
                    f"pages differ; one program may differ in at most {limit})"
                )
                continue
            patched.append({
                "index": i,
                "pages_differing": len(pages),
                "pages_total": len(base),
                "page_addresses": [page_address(first, n) for n in pages],
            })
    if differing:
        patched = []

    track_counts = {p.get("track_count") for p in probes}
    if len(track_counts) > 1:
        warnings.append(
            "discs have different track counts "
            f"({sorted(str(t) for t in track_counts)}) — [netplay] required_tracks "
            "is a single value today and will match the boot disc only"
        )

    verdict = "single" if count == 1 else ("n-programs" if differing else "data-only")
    result = {
        "disc_count": count,
        "verdict": verdict,
        "differing_program_fields": differing,
        # "identical": one file on every disc. "patched-per-disc": one program,
        # a few pages differ per disc (listed in patched_discs).
        "program_identity": (
            "distinct" if differing
            else "patched-per-disc" if patched
            else "identical"
        ),
        "patched_discs": patched,
        "warnings": warnings,
        "discs": [
            {
                "index": i,
                "cue_name": p.get("cue_name"),
                "cue_path": p.get("cue_path"),
                "serial": p.get("serial"),
                "boot_exe": p.get("boot_exe"),
                "entry_pc": p.get("entry_pc"),
                "track_count": p.get("track_count"),
                "required_disc_fp": p.get("required_disc_fp"),
            }
            for i, p in enumerate(probes, start=1)
        ],
    }
    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )

    sys.stdout.flush()
    for w in warnings:
        print(f"  warning: {w}", file=sys.stderr)
    sys.stderr.flush()

    if verdict == "n-programs":
        print()
        sys.stdout.flush()
        print(
            "error: these discs each boot their own program, which this "
            "framework cannot build yet.",
            file=sys.stderr,
        )
        print(f"  differing: {', '.join(differing)}", file=sys.stderr)
        print(
            f"  disc 1 boots {first.get('boot_exe')} at {first.get('entry_pc')}; "
            "the others do not.",
            file=sys.stderr,
        )
        print(
            "  One binary linking N recompiled programs is P2 of "
            "docs/MULTI_DISC.md, and the game emitter still emits unprefixed "
            "symbols that would collide at link.",
            file=sys.stderr,
        )
        print(
            "  Scaffold disc 1 alone (pass a single --disc) until that lands. "
            "Refusing rather than writing a one-program project that silently "
            "covers only disc 1.",
            file=sys.stderr,
        )
        return EXIT_NEEDS_N_PROGRAMS

    if verdict == "data-only":
        print()
        serials = [str(p.get("serial") or "?") for p in probes]
        if patched:
            for d in patched:
                where = ", ".join(d["page_addresses"]) or "none of the loaded image"
                print(
                    f"  note: disc {d['index']} carries disc 1's program with a "
                    f"per-disc difference: {d['pages_differing']} of "
                    f"{d['pages_total']} loaded pages differ ({where}). The "
                    "header fields and the size agree."
                )
            print(
                f"  all {count} discs boot one program, patched per disc — "
                "one program covers the set. The build is generated from disc 1."
            )
        else:
            if len(set(serials)) > 1:
                print(
                    f"  note: {len(set(serials))} serials in this set "
                    f"({', '.join(serials)}) carrying one identical executable — "
                    "normal for a multi-disc title, and why the program hash and "
                    "not the serial decides this."
                )
            print(
                f"  all {count} discs boot the same program "
                f"(sha256 {str(first.get('boot_exe_sha256') or '')[:12]}…) — one "
                f"program covers the set."
            )
        print(
            "  Scaffolding that program now. P1 has since landed: `discs` is a "
            "first-class config entry, the runtime builds a roster from it, "
            "and it mounts the SELECTED disc — remembered in disc_index — not "
            "just the boot disc. A disc can also be swapped mid-session from "
            "the in-game menu (Disc > Change disc...), which opens the lid in "
            "cdrom.c the way the game expects."
        )

    return EXIT_OK


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as e:
        if isinstance(e.code, int):
            raise
        print(f"error: {e}", file=sys.stderr)
        sys.exit(EXIT_INCOHERENT)
