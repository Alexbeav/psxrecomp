#!/usr/bin/env python3
"""Create a portable release ZIP from a staged directory.

PowerShell's Compress-Archive writes Windows backslashes into entry names.
They are tolerated by Explorer, but are not valid ZIP path separators and
break several Unix extractors. This helper always emits sorted POSIX names.

With --tar-output it also writes a gzip tarball of the same files. A ZIP's
Unix mode bits survive only when the extractor honours them, and several do
not, so Linux packages ship the tarball beside the ZIP: there every
executable (an --executable name, a file with an exec bit, an ELF image or a
#! script) is mode 0755 and every other file 0644.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import os
from pathlib import Path
import tarfile
import tempfile
import zipfile


def _zip_datetime(path: Path) -> tuple[int, int, int, int, int, int]:
    stamp = dt.datetime.fromtimestamp(path.stat().st_mtime)
    if stamp.year < 1980:
        stamp = stamp.replace(year=1980, month=1, day=1, hour=0, minute=0, second=0)
    return stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second


def _check_paths(source: Path, output: Path, kind: str) -> None:
    if not source.is_dir():
        raise ValueError(f"source is not a directory: {source}")
    try:
        output.relative_to(source)
    except ValueError:
        pass
    else:
        raise ValueError(f"output {kind} must not be inside the staged source tree")


def _release_files(source: Path) -> list[Path]:
    return sorted(
        (path for path in source.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(source).as_posix(),
    )


def _is_executable(path: Path, entry_name: str, executables: set[str]) -> bool:
    if entry_name in executables or path.stat().st_mode & 0o111:
        return True
    with path.open("rb") as stream:
        head = stream.read(4)
    return head == b"\x7fELF" or head.startswith(b"#!")


def create_release_tarball(
    source: Path, output: Path, executables: set[str] | None = None
) -> None:
    """Write a .tar.gz of the same files as the ZIP, with POSIX modes set."""
    source = source.resolve()
    output = output.resolve()
    _check_paths(source, output, "tarball")
    wanted = set(executables or ())
    files = _release_files(source)
    names = {path.relative_to(source).as_posix() for path in files}
    missing = sorted(wanted - names)
    if missing:
        raise ValueError(f"--executable not in the staged tree: {', '.join(missing)}")

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        with temporary.open("wb") as raw, gzip.GzipFile(
            filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0
        ) as compressed, tarfile.open(
            fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT
        ) as archive:
            for path in files:
                entry_name = path.relative_to(source).as_posix()
                if "\\" in entry_name or entry_name.startswith("/"):
                    raise ValueError(f"invalid tar entry name: {entry_name!r}")
                stat = path.stat()
                info = tarfile.TarInfo(entry_name)
                info.size = stat.st_size
                info.mtime = int(stat.st_mtime)
                info.mode = 0o755 if _is_executable(path, entry_name, wanted) else 0o644
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                with path.open("rb") as source_file:
                    archive.addfile(info, source_file)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def create_release_zip(source: Path, output: Path) -> None:
    source = source.resolve()
    output = output.resolve()
    _check_paths(source, output, "ZIP")

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for path in _release_files(source):
                entry_name = path.relative_to(source).as_posix()
                if "\\" in entry_name or entry_name.startswith("/"):
                    raise ValueError(f"invalid ZIP entry name: {entry_name!r}")
                info = zipfile.ZipInfo(entry_name, _zip_datetime(path))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = (path.stat().st_mode & 0xFFFF) << 16
                with path.open("rb") as source_file, archive.open(info, "w") as target:
                    while chunk := source_file.read(1024 * 1024):
                        target.write(chunk)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tar-output", type=Path,
                        help="also write a .tar.gz of the same files with POSIX modes")
    parser.add_argument("--executable", action="append", default=[],
                        help="stage-relative path that must be mode 0755 in the tarball")
    args = parser.parse_args()
    if args.executable and not args.tar_output:
        parser.error("--executable needs --tar-output")
    create_release_zip(args.source, args.output)
    print(args.output.resolve())
    if args.tar_output:
        create_release_tarball(args.source, args.tar_output, set(args.executable))
        print(args.tar_output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
