"""Read PSXRTI3 input routes (runtime/include/input_route_v3_file.h).

PSXRTI3 carries PSXRTI1/PSXRTI2 records behind a tagged extension block with
route identity (framework pin, disc serial and digest, BIOS stem, boot mode),
MENU/GAMEPLAY markers and RAM checkpoints. PSXRTI1 and PSXRTI2 files are not
PSXRTI3; read those with tools/tasreplays/run_native.route_identity.

    python tools/input_route_v3.py show ROUTE
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

HEADER = struct.Struct('<8sIIIII')
MAX_FRAMES = 1_000_000
MAX_EXT = 16 << 20
PAGES = 512
TEXT_TAGS = {0x80000101: 'pin', 0x80000102: 'disc_serial', 0x80000104: 'bios_stem',
             0x80000105: 'boot_mode'}
DIGEST_TAG, MARKER_TAG, CHECKPOINT_TAG = 0x80000103, 0x80000110, 0x80000111
CHECKPOINT_BYTES = 48 + 8 * PAGES
DIGEST_KINDS = {1: 'cue', 2: 'file'}
MARKER_KINDS = {1: 'menu', 2: 'gameplay'}
BOOT_MODES = {'lle', 'hle', 'hle-calls', 'hle-boot'}


def read(path):
    """Parse and validate a PSXRTI3 file. Raises ValueError on any fault."""
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise ValueError('short header')
    magic, version, record_size, frames, flags, ext = HEADER.unpack_from(data)
    if magic != b'PSXRTI3\0' or version != 3:
        raise ValueError('header identity')
    if record_size not in (8, 12) or not 0 < frames <= MAX_FRAMES or flags:
        raise ValueError('record size, frame count or flags')
    if ext % 4 or ext > MAX_EXT or len(data) != HEADER.size + ext + frames * record_size:
        raise ValueError('extension size or file size')
    route = {'format': 'PSXRTI3', 'record_size': record_size, 'frames': frames,
             'identity': None, 'markers': [], 'checkpoints': []}
    identity, offset, end = {}, HEADER.size, HEADER.size + ext
    while offset < end:
        if end - offset < 8:
            raise ValueError('short extension entry')
        tag, length = struct.unpack_from('<II', data, offset)
        offset += 8
        padded = (length + 3) & ~3
        if padded > end - offset:
            raise ValueError('extension length')
        payload = data[offset:offset + length]
        if any(data[offset + length:offset + padded]):
            raise ValueError('extension padding')
        offset += padded
        if tag in TEXT_TAGS:
            name = TEXT_TAGS[tag]
            if name in identity or not 0 < length < 128 or any(b < 0x20 or b > 0x7e for b in payload):
                raise ValueError(f'identity {name}')
            identity[name] = payload.decode('ascii')
        elif tag == DIGEST_TAG:
            if 'disc_digest' in identity or length != 36 or payload[0] not in DIGEST_KINDS or any(payload[1:4]):
                raise ValueError('disc digest')
            identity['disc_digest_kind'] = DIGEST_KINDS[payload[0]]
            identity['disc_digest'] = payload[4:].hex()
        elif tag == MARKER_TAG:
            if length != 8 or payload[4] not in MARKER_KINDS or any(payload[5:]):
                raise ValueError('marker')
            frame = struct.unpack_from('<I', payload)[0]
            if frame > frames or (route['markers'] and frame <= route['markers'][-1]['frame']):
                raise ValueError('marker frame')
            route['markers'].append({'frame': frame, 'kind': MARKER_KINDS[payload[4]]})
        elif tag == CHECKPOINT_TAG:
            if length != CHECKPOINT_BYTES:
                raise ValueError('checkpoint length')
            frame, zero, cycle = struct.unpack_from('<IIQ', payload)
            checkpoints = route['checkpoints']
            if zero or frame > frames or (checkpoints and frame <= checkpoints[-1]['frame']):
                raise ValueError('checkpoint')
            checkpoints.append({'frame': frame, 'cycle': cycle, 'ram_sha256': payload[16:48].hex(),
                                'pages': list(struct.unpack_from(f'<{PAGES}Q', payload, 48))})
        elif not tag & 0x80000000 or (tag & 0x7fffff00) == 0x100:
            raise ValueError(f'unsupported extension tag 0x{tag:08x}')
    if identity:
        if len(identity) != 6 or not re.fullmatch('[0-9a-f]{40}', identity['pin']) or \
                identity['boot_mode'] not in BOOT_MODES:
            raise ValueError('identity')
        route['identity'] = identity
    marker_frames = {m['frame'] for m in route['markers']}
    if any(c['frame'] not in marker_frames for c in route['checkpoints']):
        raise ValueError('checkpoint without marker')
    rows, steps, previous = offset, 0, None
    for index in range(frames):
        record = data[rows + index * record_size:rows + (index + 1) * record_size]
        sequence = struct.unpack_from('<I', record)[0]
        body = record[4:]
        if sequence != index + 1 or (record_size == 8 and any(body[2:])) or \
                (record_size == 12 and (body[6] > 1 or body[7])):
            raise ValueError('record sequence/reserved')
        steps += body != previous
        previous = body
    route['steps'] = steps
    route['records_sha256'] = hashlib.sha256(data[rows:]).hexdigest()
    return route


def _file_sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def disc_digest(path):
    """(kind, hex) exactly as the runtime computes the route disc identity."""
    path = Path(path)
    whole = _file_sha256(path)
    if path.suffix.lower() != '.cue':
        return 'file', whole
    parts = [whole]
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if re.match(r'\s*FILE\b', line, re.I):
            match = re.fullmatch(r'\s*FILE\s+"([^"\r\n]+)"\s+BINARY\s*', line, re.I)
            if not match:
                raise ValueError('cue FILE line is not a quoted BINARY track')
            parts.append(_file_sha256(path.parent / match[1]))
    if len(parts) == 1:
        raise ValueError('cue sheet has no tracks')
    return 'cue', hashlib.sha256('\n'.join(parts).encode('ascii')).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    show = sub.add_parser('show', help='print identity, markers and counts as JSON')
    show.add_argument('route', type=Path)
    args = parser.parse_args(argv)
    route = read(args.route)
    for checkpoint in route['checkpoints']:
        del checkpoint['pages']
    json.dump(route, sys.stdout, indent=2)
    sys.stdout.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
