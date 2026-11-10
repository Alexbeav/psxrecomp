"""Convert a BizHawk 2.10 Octoshock two-digital-pad movie with tray/disc events to PSXRTI3.

Profile (Abe's Exoddus 6672M): Core Octoshock, emuVersion "Version 2.10", FIOConfig Devices8
[1,0,0,0,1,0,0,0] (a digital pad on each port), no multitap, no memory cards, no embedded state.
LogKey "#Disc Select|Open|Close|Reset|#P1 <14 buttons>|#P2 <14 buttons>|".

Input rows are read the way BizHawk 2.10 reads them (Bk2Controller.SetFromMnemonic): controls
in LogKey order, any run of '|' skipped before each control, an axis parsed with int.Parse up to
the next ',', a button pressed when its character is not '.', and characters after the last
control ignored. The published movie has rows that only this reading makes sense of (a doubled
leading '|', a trailing '#' or ']', a row with its second frame appended, a missing final '|').

Console columns become PSXRTI3 console events by replaying Octoshock.FrameAdvance_PrepDiscState
and the soft-reset check (Octoshock.cs, BizHawk 2.10) over the rows, so an event is written only
where the wrapper calls into the core:
  - Open pressed with the tray closed -> TRAY_OPEN
  - Disc Select differing from the mounted disc while the tray is open -> DISC_SELECT
  - Close pressed with the tray open -> TRAY_CLOSE
  - Reset pressed -> RESET (applied after the frame's input is latched)
Row i (0-based) is wrapper Frame i, whose events apply before input record i+1 is consumed,
so they carry PSXRTI3 frame i. Frame 0 always opens the tray, mounts Disc Select and closes it
again; that is the cold state every Octoshock movie starts from (disc 1, tray closed), so it is
checked (Disc Select 1, Open not pressed) and not written as events.

Output layout (PSXRTI3 T98 tags, spec on PSX Work T98):
  0x201 port layout: two digital pads; record = u32 sequence, P1 u16 active-low + u16 0,
        P2 u16 active-low + u16 0 (12 bytes)
  0x202 console events; 0x203 disc set (kind 1 = run_native.checkpoint_asset_digest of each cue)
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile

MAX_BYTES = 64 * 1024 * 1024
MAX_FRAMES = 1000000
MAGIC = b'PSXRTI3\0'
HEADER = struct.Struct('<8sIIIII')
EXT_LIMIT = 16 * 1024 * 1024
TAG_PORT_LAYOUT, TAG_CONSOLE_EVENTS, TAG_DISC_SET = 0x201, 0x202, 0x203
DEVICE_NONE, DEVICE_DIGITAL, DEVICE_DUALSHOCK = 0, 1, 2
EVENT_RESET, EVENT_TRAY_OPEN, EVENT_TRAY_CLOSE, EVENT_DISC_SELECT = 1, 2, 3, 4
EVENT_NAMES = {EVENT_RESET: 'RESET', EVENT_TRAY_OPEN: 'TRAY_OPEN', EVENT_TRAY_CLOSE: 'TRAY_CLOSE',
               EVENT_DISC_SELECT: 'DISC_SELECT'}
DIGEST_CUE_ASSET = 1

MEMBERS = {'BizState 1.0', 'BizVersion.txt', 'Header.txt', 'Comments.txt', 'Subtitles.txt',
           'SyncSettings.json', 'Input Log.txt'}
BUTTONS = ('Up', 'Down', 'Left', 'Right', 'Select', 'Start', 'Square',
           'Triangle', 'Circle', 'Cross', 'L1', 'R1', 'L2', 'R2')
# Octoshock SetInput gamepad bits (same as bk2_intake.BITS): Select 0, Start 3, Up 4, Right 5,
# Down 6, Left 7, L2 8, R2 9, L1 10, R1 11, Triangle 12, Circle 13, Cross 14, Square 15.
BITS = (4, 6, 7, 5, 0, 3, 15, 12, 13, 14, 10, 11, 8, 9)
CONSOLE = ('Disc Select', 'Open', 'Close', 'Reset')
LOG_KEY = ('LogKey:#' + '|'.join(CONSOLE) + '|#' + '|'.join('P1 ' + b for b in BUTTONS) + '|#' +
           '|'.join('P2 ' + b for b in BUTTONS) + '|')
SYNC = {'o': {'$type': 'BizHawk.Emulation.Cores.Sony.PSX.Octoshock+SyncSettings, BizHawk.Emulation.Cores',
              'EnableLEC': False,
              'FIOConfig': {'Multitaps': [False, False], 'Memcards': [False, False],
                            'Devices8': [1, 0, 0, 0, 1, 0, 0, 0]}}}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def controls_from_log_key(log_key):
    """The LogKey's controls in order, as Bk2ControllerDefinition splits them."""
    if not log_key.startswith('LogKey:'):
        raise ValueError('missing LogKey')
    groups = [g for g in log_key[len('LogKey:'):].split('#') if g]
    return [name for group in groups for name in group.split('|') if name]


def parse_row(row, controls):
    """Bk2Controller.SetFromMnemonic for this profile: {'Disc Select': int, button: bool}."""
    values, i = {}, 0
    for name in controls:
        while i < len(row) and row[i] == '|':
            i += 1
        if i >= len(row):
            raise ValueError('row ends before its controls')  # BizHawk would throw here too
        if name == 'Disc Select':
            comma = row.find(',', i)
            if comma < 0:
                raise ValueError('axis without a comma')
            text = row[i:comma]
            if not text.strip().lstrip('+-').isdigit():
                raise ValueError(f'unparseable axis value {text!r}')
            values[name] = int(text.strip())
            i = comma + 1
        else:
            values[name] = row[i] != '.'
            i += 1
    return values, row[i:]


def load(movie_bytes):
    if len(movie_bytes) > MAX_BYTES:
        raise ValueError('movie exceeds the size limit')
    with zipfile.ZipFile(io.BytesIO(movie_bytes)) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != MEMBERS:
            raise ValueError('unsupported movie members; cold 2.10 input-only movie required')
        if sum(i.file_size for i in archive.infolist()) > 4 * MAX_BYTES:
            raise ValueError('expanded movie exceeds the size limit')
        parts = {n: archive.read(n) for n in names}
    header = {}
    for line in parts['Header.txt'].decode('utf-8-sig').splitlines():
        if line.strip():
            key, sep, value = line.partition(' ')
            if not sep or key in header:
                raise ValueError('malformed or duplicate header field')
            header[key] = value
    for key, value in {'Platform': 'PSX', 'Core': 'Octoshock', 'emuVersion': 'Version 2.10'}.items():
        if header.get(key) != value:
            raise ValueError('unsupported ' + key)
    if any(key.lower().startswith('startsfrom') for key in header):
        raise ValueError('state/save anchored movie is unsupported')
    if parts['BizState 1.0'] != b'2\r\n' or parts['BizVersion.txt'] != b'Version 2.10\r\n':
        raise ValueError('unsupported BizState/BizVersion')
    if json.loads(parts['SyncSettings.json'].decode('utf-8-sig')) != SYNC:
        raise ValueError('unsupported synchronization configuration')
    lines = parts['Input Log.txt'].decode('utf-8-sig').splitlines()
    if len(lines) < 3 or lines[0] != '[Input]' or lines[1] != LOG_KEY or lines[-1] != '[/Input]':
        raise ValueError('unsupported input framing or LogKey')
    body = lines[2:-1]
    if any(not line.startswith('|') for line in body):
        raise ValueError('non-frame line inside the input log')  # Bk2Movie would skip it; refuse
    return header, parts, body


def convert(movie_bytes, disc_digests):
    """Return (PSXRTI3 bytes, receipt). disc_digests: kind-1 digest hex per disc, roster order."""
    header, parts, body = load(movie_bytes)
    if not 0 < len(body) <= MAX_FRAMES:
        raise ValueError('frame count outside route capacity')
    if not disc_digests or any(len(bytes.fromhex(d)) != 32 for d in disc_digests):
        raise ValueError('disc set must list one 32-byte digest per disc')
    controls = controls_from_log_key(LOG_KEY)
    records, events, irregular = bytearray(), [], []
    words = {1: bytearray(), 2: bytearray()}
    tray_open, mounted = False, None
    for frame, row in enumerate(body):
        values, rest = parse_row(row, controls)
        # The well-formed row is '|DDDDD,OCR|<14>|<14>|' (41 characters); record every other
        # spelling with what BizHawk ignored, so the receipt shows how each was read.
        if rest.strip('|') or row.count('|') != 4 or len(row) != 41:
            irregular.append({'frame': frame, 'row': row, 'ignored_tail': rest})
        select = values['Disc Select']
        if frame == 0:
            if values['Open'] or select != 1:
                raise ValueError('frame 0 must mount disc 1 with the tray closed')
            tray_open, mounted = False, 1
        else:
            if values['Open'] and not tray_open:
                tray_open = True
                events.append((frame, EVENT_TRAY_OPEN, 0))
            if select != mounted and tray_open:
                if not 1 <= select <= len(disc_digests):
                    raise ValueError(f'frame {frame}: Disc Select {select} outside the disc set')
                mounted = select
                events.append((frame, EVENT_DISC_SELECT, select))
            if values['Close'] and tray_open:
                tray_open = False
                events.append((frame, EVENT_TRAY_CLOSE, mounted))
        if values['Reset']:
            events.append((frame, EVENT_RESET, 0))
        record = [frame + 1]
        for port in (1, 2):
            word = 0xFFFF
            for name, bit in zip(BUTTONS, BITS):
                if values[f'P{port} {name}']:
                    word &= ~(1 << bit)
            words[port] += struct.pack('<H', word)
            record += [word, 0]
        records += struct.pack('<IHHHH', *record)
    if tray_open:
        raise ValueError('movie ends with the tray open')
    ext = bytearray()

    def tlv(tag, payload):
        ext.extend(struct.pack('<II', tag, len(payload)))
        ext.extend(payload)
        ext.extend(b'\0' * (-len(payload) % 4))

    tlv(TAG_PORT_LAYOUT, bytes([2, 0, 0, 0, DEVICE_DIGITAL, DEVICE_DIGITAL]))
    tlv(TAG_CONSOLE_EVENTS, struct.pack('<I', len(events)) +
        b''.join(struct.pack('<IBBxx', f, e, d) for f, e, d in events))
    tlv(TAG_DISC_SET, struct.pack('<I', len(disc_digests)) +
        b''.join(bytes([DIGEST_CUE_ASSET, 0, 0, 0]) + bytes.fromhex(d) for d in disc_digests))
    if len(ext) > EXT_LIMIT:
        raise ValueError('extension block exceeds its limit')
    output = HEADER.pack(MAGIC, 3, 12, len(body), 0, len(ext)) + bytes(ext) + bytes(records)
    receipt = {
        'schema': 'octoshock210-psxrti3-route-v1', 'format': 'PSXRTI3',
        'movie_sha256': sha(movie_bytes), 'header': header,
        'members': {k: {'bytes': len(v), 'sha256': sha(v)} for k, v in sorted(parts.items())},
        'frame_count': len(body), 'record_size': 12, 'ports': ['digital', 'digital'],
        'p1_words_le_sha256': sha(bytes(words[1])), 'p2_words_le_sha256': sha(bytes(words[2])),
        'p2_pressed_frames': sum(1 for i in range(0, len(words[2]), 2) if words[2][i:i + 2] != b'\xff\xff'),
        'console_events': [{'frame': f, 'event': EVENT_NAMES[e], 'disc_index': d} for f, e, d in events],
        'disc_set': [{'index': i + 1, 'kind': 'cue_asset_digest', 'digest': d} for i, d in enumerate(disc_digests)],
        'irregular_rows': irregular,
        'row_reading': 'Bk2Controller.SetFromMnemonic (BizHawk 2.10)',
        'event_model': 'Octoshock.FrameAdvance_PrepDiscState + soft reset (BizHawk 2.10); frame 0 cold mount not written',
        'output_bytes': len(output), 'output_sha256': sha(output),
    }
    return output, receipt


def read_psxrti3(data):
    """Strict reader for the files this tool writes (the T98 subset of PSXRTI3).

    record_size never identifies the layout: the two-digital-pad record written here and a
    PSXRTI2 DualShock record are both 12 bytes. The 0x201 port-layout tag decides, and a file
    without it is refused rather than guessed at (the "8 = PSXRTI1, 12 = PSXRTI2" fallback
    belongs to readers of tag-less v3 files, not here).
    """
    if len(data) < HEADER.size:
        raise ValueError('short header')
    magic, version, record_size, count, flags, ext_bytes = HEADER.unpack_from(data)
    if magic != MAGIC or version != 3 or flags:
        raise ValueError('header identity')
    if ext_bytes % 4 or ext_bytes > EXT_LIMIT or not 0 < count <= MAX_FRAMES:
        raise ValueError('extension size or frame count')
    if HEADER.size + ext_bytes + count * record_size != len(data):
        raise ValueError('file size does not match the header')
    tags, offset, end = {}, HEADER.size, HEADER.size + ext_bytes
    while offset < end:
        tag, length = struct.unpack_from('<II', data, offset)
        payload = data[offset + 8:offset + 8 + length]
        padding = data[offset + 8 + length:offset + 8 + length + (-length % 4)]
        if len(payload) != length or any(padding) or offset + 8 + length + (-length % 4) > end:
            raise ValueError('malformed extension')
        if tag in tags:
            raise ValueError('duplicate extension tag')
        if not tag & 0x80000000 and tag not in (TAG_PORT_LAYOUT, TAG_CONSOLE_EVENTS, TAG_DISC_SET):
            raise ValueError(f'unknown mandatory tag {tag:#x}')
        tags[tag] = payload
        offset += 8 + length + (-length % 4)
    layout = tags.get(TAG_PORT_LAYOUT)
    if layout is None or layout[0] != 2 or any(layout[1:4]) or list(layout[4:6]) != [DEVICE_DIGITAL] * 2 or any(layout[6:]):
        raise ValueError('unsupported port layout')
    if record_size != 12:
        raise ValueError('record size does not match the port layout')
    events = []
    if TAG_CONSOLE_EVENTS in tags:
        if TAG_DISC_SET not in tags:
            raise ValueError('console events require a disc set')
        payload = tags[TAG_CONSOLE_EVENTS]
        (n,) = struct.unpack_from('<I', payload)
        if len(payload) != 4 + 8 * n:
            raise ValueError('console event block size')
        last = -1
        for k in range(n):
            f, e, d, pad = struct.unpack_from('<IBBH', payload, 4 + 8 * k)
            if pad or e not in EVENT_NAMES or f < last or f >= count:
                raise ValueError('console event')
            last = f
            events.append((f, e, d))
    discs = []
    if TAG_DISC_SET in tags:
        payload = tags[TAG_DISC_SET]
        (n,) = struct.unpack_from('<I', payload)
        if not n or len(payload) != 4 + 36 * n:
            raise ValueError('disc set block size')
        for k in range(n):
            entry = payload[4 + 36 * k:40 + 36 * k]
            if entry[0] not in (1, 2) or any(entry[1:4]):
                raise ValueError('disc set entry')
            discs.append((entry[0], entry[4:].hex()))
    for f, e, d in events:
        if e in (EVENT_DISC_SELECT, EVENT_TRAY_CLOSE) and not 1 <= d <= len(discs):
            raise ValueError('event disc index outside the disc set')
        if e in (EVENT_RESET, EVENT_TRAY_OPEN) and d:
            raise ValueError('event carries a disc index it cannot use')
    rows = []
    for k in range(count):
        seq, p1, z1, p2, z2 = struct.unpack_from('<IHHHH', data, end + 12 * k)
        if seq != k + 1 or z1 or z2:
            raise ValueError('record sequence/reserved')
        rows.append((p1, p2))
    return {'frames': count, 'events': events, 'discs': discs, 'rows': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('movie', type=Path)
    parser.add_argument('--disc', action='append', type=Path, required=True,
                        help='cue of each disc, in roster (m3u) order')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.receipt.exists():
        parser.error('choose new output and receipt paths')
    import run_native
    digests = [run_native.checkpoint_asset_digest(p.resolve(strict=True)) for p in args.disc]
    output, receipt = convert(args.movie.read_bytes(), digests)
    receipt['disc_paths'] = [str(p) for p in args.disc]
    if read_psxrti3(output)['frames'] != receipt['frame_count']:
        raise SystemExit('written route does not read back')
    with args.output.open('xb') as stream:
        stream.write(output)
    with args.receipt.open('x', encoding='utf-8') as stream:
        json.dump(receipt, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: receipt[k] for k in ('frame_count', 'output_bytes', 'output_sha256', 'console_events')}))


if __name__ == '__main__':
    main()
