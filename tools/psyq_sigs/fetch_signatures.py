#!/usr/bin/env python3
"""Fetch the pinned public PsyQ signature set named in SOURCE.toml.

    fetch_signatures.py --dest DIR [--archive already-downloaded.tar.gz]

Downloads the archive of the pinned commit (or uses --archive), checks its
size and SHA-256 against SOURCE.toml, and extracts the SDK version folders
(260, 300, ... 470) straight into DIR, which psyq_sigs.py takes as --sigs.
Only regular files ending in .json are extracted; nothing outside DIR is
written. Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys
import tarfile
import tomllib
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent


def load_source() -> dict:
    return tomllib.loads((HERE / "SOURCE.toml").read_text(encoding="utf-8"))["signatures"]


def check(data: bytes, src: dict) -> None:
    if len(data) != src["archive_bytes"]:
        raise SystemExit(f"archive is {len(data)} bytes, SOURCE.toml pins {src['archive_bytes']}")
    sha = hashlib.sha256(data).hexdigest()
    if sha != src["archive_sha256"]:
        raise SystemExit(f"archive sha256 {sha} does not match SOURCE.toml")


def extract(archive: pathlib.Path, dest: pathlib.Path) -> int:
    dest = dest.resolve()
    n = 0
    with tarfile.open(archive, "r:gz") as tf:
        for member in tf.getmembers():
            parts = pathlib.PurePosixPath(member.name).parts
            # <top>/<sdk version>/<LIB>.json; drop the top folder.
            if (not member.isfile() or len(parts) != 3 or not parts[1].isdigit()
                    or not parts[2].endswith(".json") or ".." in parts):
                continue
            out = (dest / parts[1] / parts[2]).resolve()
            if dest not in out.parents:
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            src = tf.extractfile(member)
            assert src is not None
            out.write_bytes(src.read())
            n += 1
    return n


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", type=pathlib.Path, required=True)
    ap.add_argument("--archive", type=pathlib.Path, help="use this local copy instead of downloading")
    args = ap.parse_args(argv)
    src = load_source()
    args.dest.mkdir(parents=True, exist_ok=True)
    archive = args.archive
    if archive is None:
        archive = args.dest / f"psx_psyq_signatures-{src['commit'][:7]}.tar.gz"
        print(f"downloading {src['archive']} ({src['archive_bytes']} bytes)")
        with urllib.request.urlopen(src["archive"], timeout=120) as r:
            archive.write_bytes(r.read())
    check(archive.read_bytes(), src)
    n = extract(archive, args.dest)
    print(f"extracted {n} signature files from {src['name']}@{src['commit'][:7]} into {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
