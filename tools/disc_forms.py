"""One disc, several file forms (PS1G-63).

A kit lists the data track of its disc in ``[prepare_disc] known_sizes``,
``known_md5`` and ``known_sha1``, entry by entry. The Redump layout keeps each
track in its own file, so the first ``FILE`` of the cue is that data track and
its whole-file digest matches.

The other common layout keeps every track in ONE ``.bin`` (it is what
``chdman extractcd`` writes). The data track is then the first bytes of that
file. Setup hashed the whole file, found no listed digest and refused a correct
disc as a "wrong dump". This module finds the listed data track at the start of
such a file, and writes the sentence for a disc that really is another one.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, Optional, Sequence

RAW_SECTOR = 2352

# (size, md5, sha1) of one listed image; "" or 0 where the kit lists none.
Known = tuple[int, str, str]


def known_images(prep: dict[str, Any]) -> list[Known]:
    """The kit's listed images, paired by position across the three lists."""
    sizes = prep.get("known_sizes") or []
    md5s = prep.get("known_md5") or []
    sha1s = prep.get("known_sha1") or []
    sizes = sizes if isinstance(sizes, list) else []
    md5s = md5s if isinstance(md5s, list) else []
    sha1s = sha1s if isinstance(sha1s, list) else []
    out: list[Known] = []
    for i in range(max(len(sizes), len(md5s), len(sha1s))):
        out.append((
            int(sizes[i]) if i < len(sizes) else 0,
            str(md5s[i]).lower() if i < len(md5s) else "",
            str(sha1s[i]).lower() if i < len(sha1s) else "",
        ))
    return out


def prefix_digests(path: Path, sizes: Sequence[int]) -> dict[int, tuple[str, str]]:
    """MD5 and SHA-1 of the first N bytes of ``path`` for each N in ``sizes``,
    in one read that stops at the largest N. A size the file does not reach is
    left out."""
    wanted = sorted({int(s) for s in sizes if int(s) > 0})
    out: dict[int, tuple[str, str]] = {}
    if not wanted:
        return out
    md5, sha1 = hashlib.md5(), hashlib.sha1()
    done = 0
    with open(path, "rb") as fh:
        for target in wanted:
            while done < target:
                chunk = fh.read(min(1024 * 1024, target - done))
                if not chunk:
                    return out
                md5.update(chunk)
                sha1.update(chunk)
                done += len(chunk)
            out[target] = (md5.copy().hexdigest(), sha1.copy().hexdigest())
    return out


def track_in_single_bin(path: Path, file_size: int, known: Sequence[Known]) -> Optional[Known]:
    """The listed data track that ``path`` begins with, or None.

    Only a listed image with a size and a digest counts, the file must be
    longer than it, and both must be whole raw sectors: the rest of the file
    is then the disc's later tracks. A file that merely shares its first bytes
    with a listed image by size alone is never accepted.
    """
    candidates = [k for k in known
                  if 0 < k[0] < file_size and k[0] % RAW_SECTOR == 0 and (k[1] or k[2])]
    if not candidates or file_size % RAW_SECTOR != 0:
        return None
    digests = prefix_digests(path, [k[0] for k in candidates])
    for size, md5, sha1 in candidates:
        got = digests.get(size)
        if got and ((md5 and got[0] == md5) or (sha1 and got[1] == sha1)):
            return (size, got[0], got[1])
    return None


def refusal_sentence(prep: dict[str, Any], selected: str, size: int, sha1: str) -> str:
    """What to tell a player whose disc image is not one the kit lists.

    It names the dump the kit was made from and the file that was selected, so
    a player who owns another pressing of the same game is not sent to look
    for a "bad dump". Kept short: the setup window shows one line of it.
    """
    known = known_images(prep)
    name = str(prep.get("cue_name") or "").strip()
    if name.lower().endswith(".cue"):
        name = name[:-4]
    wanted = ""
    if known and known[0][0]:
        wanted = f"data track {known[0][0]:,} bytes"
    if name and wanted:
        need = f"{name} ({wanted})"
    else:
        need = name or wanted or "the disc image its game.toml lists"
    return (
        f"This is not the disc image this kit was made from. The kit needs {need}. "
        f"The selected {selected} is {size:,} bytes, SHA-1 {sha1}. "
        "Another pressing, revision or region of the same game is a different disc."
    )
