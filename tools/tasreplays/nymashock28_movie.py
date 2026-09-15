"""Lossless, deterministic re-encoding of a BizHawk2.8 Nymashock PSX movie
into the Nymashock2.9.1 P1 log layout for playback on the pinned host.

BizHawk2.9.1 changed the Nymashock controller definition (Disk Index axis,
Open/Close Tray console bools, byte-range sticks in LY,LX,RY,RX order), and
Bk2Controller.SetFromMnemonic skips log-key names unknown to the current
definition without consuming mnemonic characters, so a 2.8 log cannot be
played as-is. Only the input log is rewritten. Header.txt is copied byte for
byte (emuVersion Version 2.8 records the movie's provenance and is not
forged); Comments, Subtitles and SyncSettings are copied byte for byte.

Axes: 2.9.1 writes its log byte into the high byte of the same u16 field that
2.8 wrote whole, so a 2.8 value v16 has the exact 2.9.1 spelling v16>>8 only
when v16&0xFF==0. Anything else is refused rather than rounded. Console
events, other ports and anchored movies are refused.
"""
from pathlib import Path
import argparse
import hashlib
import json
import zipfile

import dualshock_route as route

SCHEMA = 'nymashock28-relayout-v1'
EMU_VERSION_28 = 'Version 2.8'
BIZSTATE = b'2\r\n'
BIZVERSION = b'Version 2.9.1\r\n'
MEMBER_ORDER = ('BizState 1.0', 'BizVersion.txt', 'Header.txt', 'Comments.txt',
                'Subtitles.txt', 'SyncSettings.json', 'Input Log.txt')
COPIED = ('Header.txt', 'Comments.txt', 'Subtitles.txt', 'SyncSettings.json')
BUTTON_NAMES = tuple(route.LOGKEY_28.split('|')[8:25])
ZIP_DATE = (1980, 1, 1, 0, 0, 0)
COMPRESSLEVEL = 9
AXIS_MAPPING = ('2.8 u16 RX,LX,RY,LY -> 2.9.1 u8 LY,LX,RY,RX as v16>>8; '
                'exact only when v16&0xFF==0, otherwise refused')


def load_movie_28(movie):
    """Return (members, [((RX,LX,RY,LY), buttons17), ...]) of a cold 2.8 movie."""
    with zipfile.ZipFile(movie) as archive:
        names = archive.namelist()
        if len(names) != len(route.MEMBERS_28) or set(names) != route.MEMBERS_28:
            raise ValueError('Unsupported movie payload; cold 2.8 input-only movie required')
        members = {name: archive.read(name) for name in names}
    header = dict(line.split(' ', 1) for line in members['Header.txt'].decode().splitlines()
                  if ' ' in line)
    if (header.get('Core') != 'Nymashock' or header.get('Platform') != 'PSX' or
            header.get('emuVersion') != EMU_VERSION_28):
        raise ValueError('Only the declared Nymashock2.8 PSX log layout is supported')
    if any(key.startswith('StartsFrom') for key in header):
        raise ValueError('Anchored movie is not a cold controller route')
    settings = json.loads(members['SyncSettings.json'].decode())
    options = settings.get('o') if isinstance(settings, dict) else None
    if (not isinstance(options, dict) or options.get('MednafenValues') != {} or
            options.get('PortDevices') != {}):
        raise ValueError('Non-default Nymashock sync settings are unsupported')
    lines = members['Input Log.txt'].decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', route.LOGKEY_28] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    if len(lines) - 3 > route.MAX_FRAMES:
        raise ValueError('Frame capacity exceeded')
    rows = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '....':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if (len(controller) != 5 or len(controller[4]) != 17 or
                not all(value.strip().isdigit() for value in controller[:4])):
            raise ValueError('Malformed controller record')
        axes = tuple(int(value) for value in controller[:4])
        if any(not 0 <= value <= 65535 for value in axes):
            raise ValueError('Axis outside 16-bit range')
        if any(value & 0xFF for value in axes):
            raise ValueError('2.8 axis value has no exact 2.9.1 byte; lossy conversion refused')
        rows.append((axes, controller[4]))
    if not rows:
        raise ValueError('Empty controller route')
    return members, rows


def relayout_log(rows):
    """Return the 2.9.1 'Input Log.txt' bytes and per-button press counts."""
    lines = ['[Input]', route.LOGKEY]
    counts = dict.fromkeys(BUTTON_NAMES, 0)
    for (rx, lx, ry, ly), buttons in rows:
        lines.append('|    0,....|%5d,%5d,%5d,%5d,%s|' % (ly >> 8, lx >> 8, ry >> 8, rx >> 8, buttons))
        for name, character in zip(BUTTON_NAMES, buttons):
            if character != '.':
                counts[name] += 1
    lines.append('[/Input]')
    return ('\r\n'.join(lines) + '\r\n').encode(), counts


def relayout(movie, output):
    """Validate completely, then create the 2.9.1 movie; never replace one."""
    members, rows = load_movie_28(movie)
    log, counts = relayout_log(rows)
    payload = {'BizState 1.0': BIZSTATE, 'BizVersion.txt': BIZVERSION, 'Input Log.txt': log,
               **{name: members[name] for name in COPIED}}
    with Path(output).open('xb') as stream, zipfile.ZipFile(stream, 'w') as archive:
        for name in MEMBER_ORDER:
            info = zipfile.ZipInfo(name, ZIP_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 0
            info.external_attr = 0
            archive.writestr(info, payload[name], compresslevel=COMPRESSLEVEL)
    return {'schema': SCHEMA,
            'input_sha256': hashlib.sha256(Path(movie).read_bytes()).hexdigest(),
            'output_sha256': hashlib.sha256(Path(output).read_bytes()).hexdigest(),
            'rows': len(rows), 'axis_mapping': AXIS_MAPPING, 'buttons': counts,
            'console_events': 0, 'lossless': True,
            'header_emu_version': EMU_VERSION_28, 'container_version': BIZVERSION.decode().strip(),
            'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def verify(original, relayout):
    """Prove the 2.9.1 movie decodes row for row to the 2.8 movie and copies the rest."""
    expected = route.read_movie_28(original)
    actual = route.read_movie(relayout, emu_version=EMU_VERSION_28)
    if len(expected) != len(actual):
        raise ValueError('Row count differs: 2.8 %d vs 2.9.1 %d' % (len(expected), len(actual)))
    for index, (want, got) in enumerate(zip(expected, actual), 1):
        if want != got:
            raise ValueError('Row %d differs: 2.8 %r vs 2.9.1 %r' % (index, want, got))
    with zipfile.ZipFile(original) as source, zipfile.ZipFile(relayout) as target:
        if set(source.namelist()) != route.MEMBERS_28 or set(target.namelist()) != route.MEMBERS:
            raise ValueError('Unexpected movie members')
        if target.read('BizState 1.0') != BIZSTATE or target.read('BizVersion.txt') != BIZVERSION:
            raise ValueError('Unexpected 2.9.1 container version members')
        for name in COPIED:
            if source.read(name) != target.read(name):
                raise ValueError('%s is not byte-identical' % name)
    return {'rows': len(expected), 'verified': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('movie', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    receipt = relayout(args.movie, args.output)
    receipt.update(verify(args.movie, args.output))
    text = json.dumps(receipt, indent=2) + '\n'
    if args.receipt is not None:
        with args.receipt.open('x', encoding='utf-8') as stream:
            stream.write(text)
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
