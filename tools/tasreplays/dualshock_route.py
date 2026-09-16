"""Lossless, bounded PSXRTI2 export for the Nymashock2.9.1 P1 log layout.

This exports controller input only. Core, BIOS, disc, card and return-clock
qualification belongs to the caller. Console events and other ports are not
represented and are rejected. Physical Analog is retained, not interpreted
as a command to force the guest-owned controller protocol mode.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import zipfile

MAX_FRAMES = 1_000_000
# Must equal INPUT_ROUTE_MAX_STEPS in runtime/include/input_route_file.h; the
# runtime refuses a route past it. See that header for why it moved off 4096.
MAX_STEPS = 65536
HEADER = struct.Struct('<8sIIII')
RECORD = struct.Struct('<IH6B')
CONTROLLER = struct.Struct('<H5B')
LOGKEY = ('LogKey:#Disk Index|Power|Reset|Open Tray|Close Tray|'
          '#P1 Left Stick Up / Down|P1 Left Stick Left / Right|'
          'P1 Right Stick Up / Down|P1 Right Stick Left / Right|'
          'P1 D-Pad Up|P1 D-Pad Down|P1 D-Pad Left|P1 D-Pad Right|'
          'P1 Select|P1 Start|P1 △|P1 X|P1 □|P1 ○|P1 L1|P1 L2|'
          'P1 R1|P1 R2|P1 Left Stick, Button|P1 Right Stick, Button|P1 Analog|')
BUTTON_BITS = (4, 6, 7, 5, 0, 3, 12, 14, 15, 13, 10, 8, 11, 9, 1, 2)
MEMBERS = {'BizState 1.0', 'BizVersion.txt', 'Header.txt', 'Comments.txt',
           'Subtitles.txt', 'SyncSettings.json', 'Input Log.txt'}
# BizHawk2.8 Nymashock P1 layout: four console bools, then RX,LX,RY,LY as u16
# and the same seventeen button characters. 2.9.1 lands its log byte in the
# high byte of the same u16 field, so 2.8 v16 is exactly v16>>8 only when the
# low byte is zero; anything else has no lossless 2.9.1 spelling.
LOGKEY_28 = ('LogKey:#Power|Reset|Previous Disk|Next Disk|'
             '#P1 Right Stick Left / Right|P1 Left Stick Left / Right|'
             'P1 Right Stick Up / Down|P1 Left Stick Up / Down|'
             'P1 D-Pad Up|P1 D-Pad Down|P1 D-Pad Left|P1 D-Pad Right|'
             'P1 Select|P1 Start|P1 △|P1 X|P1 □|P1 ○|P1 L1|P1 L2|'
             'P1 R1|P1 R2|P1 Left Stick, Button|P1 Right Stick, Button|P1 Analog|')
MEMBERS_28 = {'Header.txt', 'Comments.txt', 'Subtitles.txt', 'SyncSettings.json', 'Input Log.txt'}
# BizHawk 2.7 Octoshock with a DualShock on port 1 (FIOConfig Devices8[0] = 2): a disc-select
# number and Open/Close/Reset, then LX,LY,RX,RY and seventeen button characters in IO_Dualshock
# order. Octoshock hands the stick bytes to the pad unscaled (Nymashock rescales them), and the
# MODE check happens only when DTR drops; neither difference can show while every stick is 128
# and MODE is never pressed, so only those records have an exact PSXRTI2 spelling.
LOGKEY_OCTO27_DUALSHOCK = ('LogKey:#Disc Select|Open|Close|Reset|'
                           '#P1 LStick X|P1 LStick Y|P1 RStick X|P1 RStick Y|'
                           'P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|'
                           'P1 Square|P1 Triangle|P1 Circle|P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|'
                           'P1 L3|P1 R3|P1 MODE|')
BUTTON_BITS_OCTO = (4, 6, 7, 5, 0, 3, 15, 12, 13, 14, 10, 11, 8, 9, 1, 2)


def read_movie(movie, emu_version='Version 2.9.1', container_version='Version 2.9.1'):
    """Return (buttons-active-low, LY,LX,RY,RX,physical-Analog) per input.

    emu_version is the declared header provenance; a 2.8 movie re-encoded
    into this layout keeps its original header, so its reader passes 'Version 2.8'.
    container_version is the BizVersion.txt marker the host wrote; a 2.10 Nymashock
    movie uses the same log key and container layout as 2.9.1, only this marker differs.
    """
    with zipfile.ZipFile(movie) as archive:
        if len(archive.namelist()) != len(MEMBERS) or set(archive.namelist()) != MEMBERS:
            raise ValueError('Unsupported movie payload; cold input-only movie required')
        if (archive.read('BizState 1.0') != b'2\r\n' or
                archive.read('BizVersion.txt') != (container_version + '\r\n').encode()):
            raise ValueError('Unsupported movie container version')
        header = dict(line.split(' ', 1) for line in archive.read('Header.txt').decode().splitlines()
                      if ' ' in line)
        if (header.get('Core') != 'Nymashock' or header.get('Platform') != 'PSX' or
                header.get('emuVersion') != emu_version):
            raise ValueError('Only the declared Nymashock2.9.1 PSX log layout is supported')
        for field in ('StartsFromSavestate', 'StartsFromSaveRam', 'StartsFromSaveRAM'):
            if header.get(field, '').lower() not in ('', 'false', '0'):
                raise ValueError('Anchored movie is not a cold controller route')
        lines = archive.read('Input Log.txt').decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    rows = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '    0,....':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if len(controller) != 5 or len(controller[4]) != 17:
            raise ValueError('Malformed controller record')
        axes = tuple(int(value) for value in controller[:4])
        if any(not 0 <= value <= 255 for value in axes):
            raise ValueError('Axis outside byte range')
        word = 0xFFFF
        for character, bit in zip(controller[4][:16], BUTTON_BITS):
            if character != '.':
                word &= ~(1 << bit)
        rows.append((word, *axes, int(controller[4][16] != '.')))
        if len(rows) > MAX_FRAMES:
            raise ValueError('Frame capacity exceeded')
    if not rows:
        raise ValueError('Empty controller route')
    return rows


def read_movie_28(movie):
    """Return read_movie rows directly from a cold BizHawk2.8 Nymashock P1 movie."""
    with zipfile.ZipFile(movie) as archive:
        if len(archive.namelist()) != len(MEMBERS_28) or set(archive.namelist()) != MEMBERS_28:
            raise ValueError('Unsupported movie payload; cold 2.8 input-only movie required')
        header = dict(line.split(' ', 1) for line in archive.read('Header.txt').decode().splitlines()
                      if ' ' in line)
        if (header.get('Core') != 'Nymashock' or header.get('Platform') != 'PSX' or
                header.get('emuVersion') != 'Version 2.8'):
            raise ValueError('Only the declared Nymashock2.8 PSX log layout is supported')
        if any(field.startswith('StartsFrom') for field in header):
            raise ValueError('Anchored movie is not a cold controller route')
        settings = json.loads(archive.read('SyncSettings.json').decode())
        options = settings.get('o') if isinstance(settings, dict) else None
        if (not isinstance(options, dict) or options.get('MednafenValues') != {} or
                options.get('PortDevices') != {}):
            raise ValueError('Non-default Nymashock sync settings are unsupported')
        lines = archive.read('Input Log.txt').decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY_28] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    rows = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '....':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if (len(controller) != 5 or len(controller[4]) != 17 or
                not all(value.strip().isdigit() for value in controller[:4])):
            raise ValueError('Malformed controller record')
        rx, lx, ry, ly = (int(value) for value in controller[:4])
        if any(not 0 <= value <= 65535 for value in (rx, lx, ry, ly)):
            raise ValueError('Axis outside 16-bit range')
        if any(value & 0xFF for value in (rx, lx, ry, ly)):
            raise ValueError('2.8 axis value has no exact 2.9.1 byte; lossy conversion refused')
        word = 0xFFFF
        for character, bit in zip(controller[4][:16], BUTTON_BITS):
            if character != '.':
                word &= ~(1 << bit)
        rows.append((word, ly >> 8, lx >> 8, ry >> 8, rx >> 8, int(controller[4][16] != '.')))
        if len(rows) > MAX_FRAMES:
            raise ValueError('Frame capacity exceeded')
    if not rows:
        raise ValueError('Empty controller route')
    return rows


def read_movie_octoshock27(movie):
    """Return read_movie rows from a cold BizHawk 2.7 Octoshock movie with a DualShock on P1.

    One disc, never ejected or reset; no multitap or memory card; both sticks neutral and
    MODE never pressed (see LOGKEY_OCTO27_DUALSHOCK for why only those records are exact)."""
    with zipfile.ZipFile(movie) as archive:
        if len(archive.namelist()) != len(MEMBERS_28) or set(archive.namelist()) != MEMBERS_28:
            raise ValueError('Unsupported movie payload; cold 2.7 input-only movie required')
        header = dict(line.split(' ', 1) for line in archive.read('Header.txt').decode().splitlines()
                      if ' ' in line)
        if (header.get('Core') != 'Octoshock' or header.get('Platform') != 'PSX' or
                header.get('emuVersion') != 'Version 2.7.0'):
            raise ValueError('Only the declared Octoshock 2.7 PSX log layout is supported')
        if any(field.startswith('StartsFrom') for field in header):
            raise ValueError('Anchored movie is not a cold controller route')
        settings = json.loads(archive.read('SyncSettings.json').decode())
        options = settings.get('o') if isinstance(settings, dict) else None
        fio = options.get('FIOConfig') if isinstance(options, dict) else None
        if (not isinstance(fio, dict) or fio.get('Devices8') != [2, 0, 0, 0, 0, 0, 0, 0] or
                fio.get('Memcards') != [False, False] or fio.get('Multitaps') != [False, False]):
            raise ValueError('Only a lone DualShock on port 1 with no card or multitap is supported')
        lines = archive.read('Input Log.txt').decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY_OCTO27_DUALSHOCK] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    rows = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '    1,...':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if (len(controller) != 5 or len(controller[4]) != 17 or
                not all(value.strip().isdigit() for value in controller[:4])):
            raise ValueError('Malformed controller record')
        if any(int(value) != 128 for value in controller[:4]) or controller[4][16] != '.':
            raise ValueError('Octoshock stick or MODE input has no exact PSXRTI2 spelling')
        word = 0xFFFF
        for character, bit in zip(controller[4][:16], BUTTON_BITS_OCTO):
            if character != '.':
                word &= ~(1 << bit)
        rows.append((word, 128, 128, 128, 128, 0))
        if len(rows) > MAX_FRAMES:
            raise ValueError('Frame capacity exceeded')
    if not rows:
        raise ValueError('Empty controller route')
    return rows


def write_route(rows, output):
    """Validate completely before creating a new route; never replace one."""
    if not 1 <= len(rows) <= MAX_FRAMES:
        raise ValueError('Frame capacity exceeded')
    steps, previous = 0, None
    digest = hashlib.sha256()
    for row in rows:
        if (len(row) != 6 or any(type(value) is not int for value in row) or
                not 0 <= row[0] <= 65535 or any(not 0 <= value <= 255 for value in row[1:5]) or
                row[5] not in (0, 1)):
            raise ValueError('Invalid complete controller state')
        if row != previous:
            steps += 1
            previous = row
        if steps > MAX_STEPS:
            raise ValueError('Step capacity exceeded')
        digest.update(CONTROLLER.pack(*row))
    with Path(output).open('xb') as stream:
        stream.write(HEADER.pack(b'PSXRTI2\0', 2, RECORD.size, len(rows), 0))
        for index, row in enumerate(rows, 1):
            stream.write(RECORD.pack(index, *row, 0))
    return {'schema': 'psxrti2-controller-route-v1', 'frames': len(rows), 'steps': steps,
            'canonical_controller_sha256': digest.hexdigest(),
            'canonical_encoding': 'LE u16 active-low buttons;u8 LY,LX,RY,RX,physical Analog',
            'qualification': 'Lossless controller encoding only; native delivery and core compatibility separate'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('movie', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    receipt = write_route(read_movie(args.movie), args.output)
    receipt['movie_sha256'] = hashlib.sha256(args.movie.read_bytes()).hexdigest()
    receipt['route_sha256'] = hashlib.sha256(args.output.read_bytes()).hexdigest()
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
