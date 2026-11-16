"""Read a PSX disc image's ISO9660 metadata without staging bin/cue.

`probe_disc` already takes its sector reader as a callable, `read_user(data, lba)`,
and never asks where `data` came from. This module supplies a `data` object that
answers sector reads from a `.chd` through libchdr, so the same probe logic works
against a compressed image with no extracted track file.

Equivalence is measured, not assumed: libchdr's track bytes are identical to
`chdman extractcd -ob` (see PSX-BUILD-031). Only the reader changes here.

Usage:
    from chd_source import ChdTrackSource
    with ChdTrackSource(chd_path, psx_chd, lib_path) as src:
        user = src.read_user(None, 16)      # 2048 bytes of LBA 16
"""
from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

RAW = 2352
USER = 2048
# MODE2/2352 Form-1 and MODE1/2352 both place 2048 user bytes after a header;
# MODE2 has an 8-byte subheader, so try 24 first.
USER_OFFSETS = (24, 16)


class ChdTrackSource:
    """A single data track of a .chd exposed as sector-addressable data."""

    def __init__(self, chd: Path, psx_chd: Any, lib_path: Path, track_number: Optional[int] = None):
        self.chd = Path(chd)
        self.psx_chd = psx_chd
        self._disc = psx_chd.ChdDisc(self.chd, psx_chd.LibChdr(lib_path))
        tracks = [t for t in self._disc.tracks if t.type in ("MODE1_RAW", "MODE2_RAW")]
        if not tracks:
            self._disc.close()
            raise ValueError(f"{self.chd.name} has no data track")
        self.track = next((t for t in tracks if t.number == track_number), tracks[0])
        self.frames = self.track.frames
        self._user_offset = self._detect_user_offset()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        if self._disc is not None:
            self._disc.close()
            self._disc = None

    def __enter__(self) -> "ChdTrackSource":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- sector access -----------------------------------------------------
    def _raw_sector(self, lba: int) -> bytes:
        """Read one 2352-byte sector of the data track by LBA."""
        if lba < 0 or lba >= self.frames:
            raise KeyError(lba)
        frame = self.track.chd_frame_start + lba
        hunk_index = frame // self._disc.frames_per_hunk
        within = frame % self._disc.frames_per_hunk
        hunk = self._disc._hunk(hunk_index)
        start = within * self.psx_chd.CD_FRAME_SIZE
        return hunk[start:start + RAW]

    def _detect_user_offset(self) -> int:
        """Find where the 2048 user bytes start in a raw sector."""
        raw = self._raw_sector(16)
        for offset in USER_OFFSETS:
            if len(raw) >= offset + USER and raw[offset + 1:offset + 6] == b"CD001":
                return offset
        # Fall back to MODE2 and let the caller's CD001 check report a problem.
        return USER_OFFSETS[0]

    def read_user(self, _data: Any, lba: int) -> bytes:
        """`read_user(data, lba)` as probe_disc expects; `data` is ignored."""
        raw = self._raw_sector(lba)
        return raw[self._user_offset:self._user_offset + USER]

    def read_data_track(self) -> bytes:
        """The whole data track, for digest computation."""
        out = bytearray()
        for chunk in self._disc.iter_track_chunks(self.track):
            out += chunk
        return bytes(out)
