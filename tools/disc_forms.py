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

Such a file says nothing about where its tracks begin: the cue does. With no
cue (or a cue that calls the whole file one track) the track list comes from
the kit, or the file is refused. A kit gives it in two optional keys::

    [prepare_disc]
    track_sizes   = [527385600, 34809600, 41395200]   # bytes of each track file
    track_pregaps = [0, 150, 150]                     # frames; this is the default

``track_sizes`` holds the size of every track as the one-file-per-track layout
stores it (a later track's file begins with its pregap). Track 1 is the data
track and every later track is CD audio. The list is used only for a file it
fits: track 1 is a listed data track and the sizes add up to the file's length.
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path
from typing import Any, Optional, Sequence

RAW_SECTOR = 2352
AUDIO_PREGAP_FRAMES = 150

# (size, md5, sha1) of one listed image; "" or 0 where the kit lists none.
Known = tuple[int, str, str]
# (track number, is audio, frame of INDEX 00, frame of INDEX 01) in the one file.
TrackEntry = tuple[int, bool, int, int]


def cue_track_count(cue_path: Path) -> int:
    """How many TRACK entries a cue declares."""
    text = cue_path.read_text(encoding="utf-8", errors="replace")
    return len(re.findall(r"^\s*TRACK\s+\d+\s+\S+", text, flags=re.I | re.M))


def owning_cue(image: Path) -> Optional[Path]:
    """The cue beside a raw image that names it as a FILE, or None.

    The runtime mounts that cue when a player picks the ``.bin``
    (``disc_path.cpp``), so setup stages the same disc: without the cue the
    track table is lost and CD audio with it. A cue that names a file that is
    not there is left alone.
    """
    image = image.resolve()
    same_stem = image.with_suffix(".cue")
    cues = [same_stem] if same_stem.is_file() else []
    cues += sorted(p for p in image.parent.iterdir()
                   if p.suffix.lower() == ".cue" and p.is_file() and p not in cues)
    for cue in cues:
        text = cue.read_text(encoding="utf-8", errors="replace")
        names = re.findall(r'FILE\s+"([^"]+)"\s+BINARY', text, flags=re.I)
        files = [(cue.parent / n).resolve() for n in names]
        if image in files and all(f.is_file() for f in files):
            return cue
    return None


def no_track_list_sentence(name: str) -> str:
    """What to tell a player who picked one file that holds every track of the
    right disc, with no cue that lists the tracks and no list in the kit."""
    return (
        f"Select the .cue file that belongs to {name}: this file holds every track "
        "of the disc in one piece, and setup needs the .cue to find them."
    )


def kit_track_table(prep: dict[str, Any], image_size: int, track1_size: int) -> Optional[list[TrackEntry]]:
    """The track list of a one-file disc from the kit's own values, or None.

    Only a list that fits the file counts: its first track is the listed data
    track and its tracks add up to the file's length. Anything else is None,
    and the caller refuses the file rather than guess where a track begins.
    """
    sizes = prep.get("track_sizes")
    pregaps = prep.get("track_pregaps")
    if not isinstance(sizes, list) or len(sizes) < 2:
        return None
    if pregaps is None:
        pregaps = [0] + [AUDIO_PREGAP_FRAMES] * (len(sizes) - 1)
    if not isinstance(pregaps, list) or len(pregaps) != len(sizes):
        return None
    if not all(type(v) is int for v in list(sizes) + list(pregaps)):
        return None
    if any(s <= 0 or s % RAW_SECTOR for s in sizes):
        return None
    if sizes[0] != track1_size or sum(sizes) != image_size:
        return None
    table: list[TrackEntry] = []
    frame = 0
    for number, (size, gap) in enumerate(zip(sizes, pregaps), start=1):
        frames = size // RAW_SECTOR
        if gap < 0 or gap >= frames:
            return None
        table.append((number, number > 1, frame, frame + gap))
        frame += frames
    return table


def _msf(frames: int) -> str:
    return f"{frames // 4500:02d}:{frames // 75 % 60:02d}:{frames % 75:02d}"


def rebuilt_cue(bin_name: str, table: Sequence[TrackEntry]) -> str:
    """Cue text for one file that holds the tracks of ``table``."""
    lines = [f'FILE "{bin_name}" BINARY']
    for number, audio, index00, index01 in table:
        lines.append(f"  TRACK {number:02d} {'AUDIO' if audio else 'MODE2/2352'}")
        if index00 != index01:
            lines.append(f"    INDEX 00 {_msf(index00)}")
        lines.append(f"    INDEX 01 {_msf(index01)}")
    return "\n".join(lines) + "\n"


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
    for a "bad dump". Setup cannot tell another pressing from a damaged copy
    or from the right disc in a form it does not read, so the last sentence
    names all three. Kept short: the setup window shows one line of it.
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
        "It may be another pressing, revision or region of the game (a different "
        "disc), a damaged copy, or the right disc in a form setup cannot read."
    )
