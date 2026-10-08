"""The track list of a selected disc against the kit's (PS1B-407).

Setup checks the data track of a disc by its digest ([prepare_disc] known_*).
A disc can have the right data track and still be incomplete: a later track is
missing, cut short or another file, or the .cue is another disc's. The game
then runs with missing or wrong CD audio, and nothing said so.

A kit already carries the facts, in [netplay]: required_tracks and
required_disc_fp (required_disc_fps for the discs of a set). The fingerprint is
"psxrecomp-toc-v1": the track count, the lead-out, and each track's type, start
and pregap (runtime/src/disc_identity.cpp; the same value from a cue is
tools/new_project_layout/probe_disc.py compute_disc_fp). A track that is cut
short moves the lead-out, so the fingerprint differs.

What it cannot see: the bytes of a later track. A later track that has the
right length and other content gives the same fingerprint. Checking that needs
a digest per track, which a kit does not carry.

What a difference does is one setting of the kit:

    [prepare_disc]
    track_list_mismatch = "warn"     # or "refuse"

"warn" (the default) accepts the disc and logs one sentence. "refuse" ends the
check the way a wrong data track does. The default changes no verdict of a
disc; which of the two a player gets is a decision for each kit.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys
from typing import Any, List, Mapping, Optional, Sequence, Tuple

_LAYOUT = Path(__file__).resolve().parent / "new_project_layout"
if str(_LAYOUT) not in sys.path:
    sys.path.insert(0, str(_LAYOUT))
import probe_disc  # noqa: E402

SETTING = "track_list_mismatch"
WARN = "warn"
REFUSE = "refuse"
DEFAULT = WARN

RAW_SECTOR = 2352
COOKED_SECTOR = 2048
SUBCODE_SECTOR = 2448


@dataclass
class TrackList:
    """What the fingerprint is made from, for one disc."""

    tracks: int
    leadout: int
    disc_fp: str


@dataclass
class KitTrackList:
    """What a kit says about its disc. `fps` holds every fingerprint the kit
    lists: its boot disc's and, for a set, each disc's."""

    fps: Tuple[str, ...]
    tracks: int                 # 0: not given
    leadout: Optional[int]      # None: not given


class Unreadable(Exception):
    """The selected disc has no track list that can be compared. The text says
    why, as the part of the sentence that follows "its track list is not the
    kit's"."""


def policy(prep: Optional[Mapping[str, Any]]) -> Tuple[str, str]:
    """(what a difference does, a note). The note is not empty when the kit's
    value is neither "warn" nor "refuse"; the default then applies, so that a
    slip in a kit's file cannot refuse a player's disc."""
    value = (prep or {}).get(SETTING)
    if value is None or str(value).strip() == "":
        return DEFAULT, ""
    text = str(value).strip().lower()
    if text in (WARN, REFUSE):
        return text, ""
    return DEFAULT, ('[prepare_disc] %s = "%s" is not "%s" or "%s"; "%s" is used'
                     % (SETTING, value, WARN, REFUSE, DEFAULT))


def _whole_number(value: Any) -> Optional[int]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return None


def kit_track_list(netplay: Optional[Mapping[str, Any]]) -> Optional[KitTrackList]:
    """The kit's track list from its [netplay] table, or None when the kit says
    nothing about its tracks."""
    netplay = netplay or {}
    listed: List[str] = []
    values = [netplay.get("required_disc_fp")]
    more = netplay.get("required_disc_fps")
    if isinstance(more, (list, tuple)):
        values += list(more)
    for value in values:
        text = str(value or "").strip().lower()
        if text and text not in listed:
            listed.append(text)
    tracks = _whole_number(netplay.get("required_tracks")) or 0
    leadout = _whole_number(netplay.get("required_leadout_lba")) if "required_leadout_lba" in netplay else None
    if not listed and tracks <= 0 and leadout is None:
        return None
    return KitTrackList(fps=tuple(listed), tracks=max(tracks, 0), leadout=leadout)


def _from_text(canonical: str) -> TrackList:
    """A TrackList from the psxrecomp-toc-v1 text. The count and the lead-out
    are read back from the text the fingerprint is made of, so the three
    cannot disagree."""
    import hashlib

    fields = dict(line.split("=", 1) for line in canonical.splitlines()[1:3])
    return TrackList(tracks=int(fields["tracks"]), leadout=int(fields["leadout"]),
                     disc_fp=hashlib.sha256(canonical.encode("ascii")).hexdigest())


def _sectors_or_why(name: str, size: int) -> None:
    if size <= 0 or (size % RAW_SECTOR and size % COOKED_SECTOR):
        raise Unreadable('"%s" is %d bytes, which is not a whole number of sectors' % (name, size))


def of_cue(cue: Path) -> TrackList:
    """The track list a .cue and its files give."""
    try:
        tracks, files = probe_disc.parse_cue(cue)
    except (SystemExit, ValueError, OSError) as error:
        raise Unreadable("the .cue cannot be read (%s)" % error) from None
    for name in files:
        shown = Path(name.replace("\\", "/")).name
        try:
            path = probe_disc.resolve_cue_file(cue, name)
        except SystemExit:
            raise Unreadable('the .cue names "%s", and that file is not there' % shown) from None
        _sectors_or_why(shown, path.stat().st_size)
    try:
        return _from_text(probe_disc.toc_canonical(cue, tracks, files))
    except (SystemExit, OSError) as error:
        raise Unreadable("the .cue cannot be read (%s)" % error) from None


def of_file(image: Path) -> TrackList:
    """A file selected without a .cue is one track: the whole file."""
    size = image.stat().st_size
    if size > 0 and size % RAW_SECTOR and size % COOKED_SECTOR and size % SUBCODE_SECTOR == 0:
        # A dump with the subchannel data after each sector. Setup stages it
        # without that data, as 2352-byte sectors.
        size = size // SUBCODE_SECTOR * RAW_SECTOR
    _sectors_or_why(image.name, size)
    name = "track01"
    track = probe_disc.CueTrack(number=1, kind="MODE2/2352", file=name, index01_frames=0)
    return _from_text(probe_disc.toc_canonical(image, [track], [name], {name: size}))


def of_chd_tracks(chd_tracks: Sequence[Any]) -> TrackList:
    """The track list of a CHD, from its track table (psx_chd.ChdTrack).

    It is the list of the cue that psx_chd.render_cue(layout="multi") writes
    for the table, with one file per track: the form the CHD is staged in and
    the form probe_disc fingerprints a CHD in. No track data is read.
    """
    tracks = []
    files = []
    sizes = {}
    for entry in chd_tracks:
        name = "track%02d" % entry.number
        files.append(name)
        sizes[name] = entry.frames * RAW_SECTOR
        tracks.append(probe_disc.CueTrack(
            number=entry.number, kind="AUDIO" if entry.is_audio else "MODE2/2352", file=name,
            index01_frames=entry.stored_pregap,
            index00_frames=0 if entry.stored_pregap else None))
    if not tracks:
        raise Unreadable("the .chd has no track table")
    try:
        return _from_text(probe_disc.toc_canonical(Path("."), tracks, files, sizes))
    except SystemExit as error:
        raise Unreadable("the .chd's track table cannot be read (%s)" % error) from None


def of_selected(disc: Path, chd_tracks: Optional[Sequence[Any]] = None) -> TrackList:
    if chd_tracks is not None:
        return of_chd_tracks(chd_tracks)
    if disc.suffix.lower() == ".cue":
        return of_cue(disc)
    return of_file(disc)


def _count(tracks: int) -> str:
    return "1 track" if tracks == 1 else "%d tracks" % tracks


def difference(kit: KitTrackList, got: TrackList) -> str:
    """"" when the selected disc's track list is the kit's. Otherwise what
    differs, as the words that follow "its track list is not the kit's"."""
    if kit.fps:
        same = got.disc_fp in kit.fps
    else:
        same = ((kit.tracks <= 0 or got.tracks == kit.tracks)
                and (kit.leadout is None or got.leadout == kit.leadout))
    if same:
        return ""
    if kit.tracks > 0 and got.tracks != kit.tracks:
        return "it has %s, the kit's disc has %d" % (_count(got.tracks), kit.tracks)
    if kit.leadout is not None and got.leadout != kit.leadout:
        return "it is %d sectors long, the kit's disc is %d" % (got.leadout, kit.leadout)
    if got.tracks == 1:
        return "its track does not end where the kit's does"
    return "its tracks do not begin and end where the kit's do"


def sentence(name: str, detail: str, *, data_track_checked: bool, refuse: bool) -> str:
    """What a player reads. It names the cause first, so that a short window
    cuts the advice and not the cause. No word of it is one the setup program
    takes for the sign of an error line ("error", "failed", "fatal"): a
    warning must not become the reason of a later, unrelated stop.
    """
    if data_track_checked:
        lead = 'the data track of "%s" is right, but the disc\'s track list is not the kit\'s' % name
    else:
        lead = 'the track list of "%s" is not the kit\'s' % name
    cause = "a track is missing, cut short or changed, or the .cue belongs to another disc"
    if refuse:
        text = "%s (%s): %s. Setup needs the complete disc." % (lead, detail, cause)
        return text[0].upper() + text[1:]
    return ("Track list: %s (%s): %s. The game can run; its CD audio can be missing or wrong."
            % (lead, detail, cause))


def check(disc: Path, netplay: Optional[Mapping[str, Any]], prep: Optional[Mapping[str, Any]], *,
          chd_tracks: Optional[Sequence[Any]] = None, data_track_checked: bool = True) -> dict:
    """Compare the selected disc's track list with the kit's. Returns the record:

    status   "match", "mismatch" or "not_checked"
    policy   "warn" or "refuse": what a mismatch does for this kit
    reason   why nothing was checked (status "not_checked")
    detail, sentence   what differs, and what a player reads (status "mismatch")
    tracks, leadout, disc_fp   the selected disc's own values, when it has a list
    kit_fault   the kit's setting is not a value this check knows
    """
    mode, note = policy(prep)
    record: dict = {"status": "not_checked", "policy": mode}
    if note:
        record["kit_fault"] = note
    kit = kit_track_list(netplay)
    if kit is None:
        record["reason"] = "the kit lists no track list ([netplay] required_disc_fp, required_tracks)"
        return record
    try:
        got = of_selected(disc, chd_tracks)
    except Unreadable as error:
        detail = str(error)
    except OSError as error:
        detail = "the selected file cannot be read (%s)" % error
    else:
        record.update(tracks=got.tracks, leadout=got.leadout, disc_fp=got.disc_fp)
        detail = difference(kit, got)
        if detail and chd_tracks is None and disc.suffix.lower() != ".cue" and kit.tracks > got.tracks:
            # The usual cause: the first track's file was selected, not the
            # .cue. Setup then stages that one file and the others are left.
            detail += "; select the disc's .cue to include every track"
    if not detail:
        record["status"] = "match"
        return record
    record.update(status="mismatch", detail=detail,
                  sentence=sentence(disc.name, detail, data_track_checked=data_track_checked,
                                    refuse=(mode == REFUSE)))
    return record
