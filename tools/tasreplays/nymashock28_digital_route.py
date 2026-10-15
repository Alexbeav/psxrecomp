"""Strict PSXRTI1 export of a cold BizHawk 2.8 Nymashock plain-gamepad movie.

The movie has `PortDevices {"0":"gamepad"}`, four console bools and fourteen
P1 button characters, with no sticks and no Analog. Anything else is refused:
another core or version, another port device, a second port, console events,
disc changes, anchored movies, non-default Mednafen values, a malformed row.
Only the controller word is exported. Core, BIOS, disc, card and return-clock
qualification belongs to the caller; this makes no claim about equivalence.

PS1G-128. DualShock 2.8 movies are read by nymashock28_movie.py and
nymashock28_route.py (PS1G-125 territory); this module shares nothing with them
but the button-bit table.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import zipfile

import dualshock_route as route

MAX_FRAMES = 1_000_000
# Must equal INPUT_ROUTE_MAX_STEPS in runtime/include/input_route_file.h.
MAX_STEPS = 65536
EMU_VERSION_28 = 'Version 2.8'
MEMBERS = route.MEMBERS_28
LOGKEY = ('LogKey:#Power|Reset|Previous Disk|Next Disk|'
          '#P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|'
          'P1 △|P1 X|P1 □|P1 ○|P1 L1|P1 L2|P1 R1|P1 R2|')
BUTTON_BITS = route.BUTTON_BITS[:14]  # Up Down Left Right Select Start tri X sq circle L1 L2 R1 R2
HEADER = struct.Struct('<8sIIII')
RECORD = struct.Struct('<IHH')


def read_movie(movie):
    """Return the active-low pad word of every row of a cold 2.8 plain-gamepad movie."""
    with zipfile.ZipFile(movie) as archive:
        names = archive.namelist()
        if len(names) != len(MEMBERS) or set(names) != MEMBERS:
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
            options.get('PortDevices') != {'0': 'gamepad'}):
        raise ValueError('Only a plain gamepad on port 1 with default Nymashock settings is supported')
    lines = members['Input Log.txt'].decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    if len(lines) - 3 > MAX_FRAMES:
        raise ValueError('Frame capacity exceeded')
    letters = [None] * len(BUTTON_BITS)
    words = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '....':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        buttons = fields[2]
        if len(buttons) != len(BUTTON_BITS):
            raise ValueError('Malformed controller record')
        word = 0xFFFF
        for index, (character, bit) in enumerate(zip(buttons, BUTTON_BITS)):
            if character == '.':
                continue
            if not character.isalnum() or not character.isascii():
                raise ValueError('Unexpected character in button record')
            if letters[index] not in (None, character):
                raise ValueError('Button %d is spelled two ways' % index)
            letters[index] = character
            word &= ~(1 << bit)
        words.append(word)
    if not words:
        raise ValueError('Empty controller route')
    return words


def encode(words):
    """Return (PSXRTI1 bytes, receipt) for the pad words; refuse what the runtime would refuse."""
    if not 1 <= len(words) <= MAX_FRAMES:
        raise ValueError('Frame capacity exceeded')
    steps, previous = 0, None
    for word in words:
        if type(word) is not int or not 0 <= word <= 0xFFFF:
            raise ValueError('Invalid pad word')
        steps += word != previous
        previous = word
    if steps > MAX_STEPS:
        raise ValueError('Step capacity exceeded')
    data = bytearray(HEADER.pack(b'PSXRTI1\0', 1, RECORD.size, len(words), 0))
    for index, word in enumerate(words, 1):
        data += RECORD.pack(index, word, 0)
    packed = b''.join(struct.pack('<H', word) for word in words)
    return bytes(data), {'schema': 'psxrti1-nymashock28-gamepad-v1', 'format': 'PSXRTI1',
                         'frames': len(words), 'steps': steps,
                         'words_sha256': hashlib.sha256(packed).hexdigest(),
                         'output_sha256': hashlib.sha256(data).hexdigest(),
                         'qualification': 'Lossless controller encoding only; native delivery and '
                                          'core compatibility separate; no equivalence claimed'}


def export(movie, output):
    """Validate completely, then create the route; never replace one."""
    data, receipt = encode(read_movie(movie))
    receipt['input_sha256'] = hashlib.sha256(Path(movie).read_bytes()).hexdigest()
    receipt['tool_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    with Path(output).open('xb') as stream:
        stream.write(data)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('movie', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    receipt = export(args.movie, args.output)
    text = json.dumps(receipt, indent=2) + '\n'
    if args.receipt is not None:
        with args.receipt.open('x', encoding='utf-8') as stream:
            stream.write(text)
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
