"""Export a BizHawk 2.8 Nymashock P1 movie to PSXRTI2, applying the pad's own axis conversion.

The 2.8 log records each stick axis as the raw u16 the frontend hands the core, so a route cannot
simply copy it: the emulated pad converts that u16 to the byte the guest reads. nymashock28_movie.py
re-encodes a 2.8 movie into the 2.9.1 layout and therefore refuses any axis whose low byte is set
(no exact 2.9.1 spelling); MediEvil 4875M and Spyro 2 5278M are full of such values, so they need
this path instead, which records the byte the pad actually received.

The conversion itself is not implemented here. external/source_dualshock_axis.py exports it as a
65,536-byte table (GPL-2.0-or-later, credited to the Mednafen commits BizHawk pins), and this tool
consumes that table as data, exactly as the native runs consume the external random tape.

Axis order differs too: 2.8 logs RX, LX, RY, LY while PSXRTI2 stores LY, LX, RY, RX. Buttons are
the same seventeen characters in the same order as 2.9.1, so their bits are dualshock_route's.

Console events (Power, Reset, Previous/Next Disk) and non-default port devices are refused. A
movie may declare MednafenValues -- Spyro 2 turns the port-1 memory card off -- and the declared
values are recorded in the receipt so the caller can check them against its source reference.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import zipfile

import dualshock_route as route

SCHEMA = 'nymashock28-psxrti2-route-v1'
EMU_VERSION = 'Version 2.8'
AXIS_ORDER = '2.8 log RX,LX,RY,LY -> PSXRTI2 LY,LX,RY,RX'
TABLE_BYTES = 1 << 16
# Accepted Mednafen settings a movie may declare. Anything else changes emulation in a way this
# encoder does not model, so it is refused rather than carried silently.
ALLOWED_MEDNAFEN_KEYS = tuple('psx.input.port%d.memcard' % port for port in range(1, 9))


def check_axis_table(data):
    if len(data) != TABLE_BYTES:
        raise ValueError('axis table must be 65,536 bytes')
    if data[0] != 0 or data[0x8000] != 128 or data[0xFFFF] != 255:
        raise ValueError('axis table endpoints differ from the pad conversion')
    return bytes(data)


def load_axis_table(path):
    return check_axis_table(Path(path).read_bytes())


def default_axis_table():
    """The table external/source_dualshock_axis.py exports, the same data its CLI writes."""
    path = Path(__file__).resolve().parent / 'external' / 'source_dualshock_axis.py'
    spec = importlib.util.spec_from_file_location('source_dualshock_axis', path)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    return check_axis_table(tool.table())


def load_movie(movie_bytes):
    """Return (header, sync options, [(axes RX,LX,RY,LY, buttons17), ...])."""
    with zipfile.ZipFile(movie_bytes) as archive:
        names = set(archive.namelist())
        if len(archive.namelist()) != len(route.MEMBERS_28) or names != route.MEMBERS_28:
            raise ValueError('Unsupported movie payload; cold 2.8 input-only movie required')
        parts = {name: archive.read(name) for name in names}
    header = dict(line.split(' ', 1) for line in parts['Header.txt'].decode().splitlines() if ' ' in line)
    if (header.get('Core') != 'Nymashock' or header.get('Platform') != 'PSX' or
            header.get('emuVersion') != EMU_VERSION):
        raise ValueError('Only the declared Nymashock 2.8 PSX log layout is supported')
    for field in ('StartsFromSavestate', 'StartsFromSaveRam', 'StartsFromSaveRAM'):
        if header.get(field, '').lower() not in ('', 'false', '0'):
            raise ValueError('Anchored movie is not a cold controller route')
    settings = json.loads(parts['SyncSettings.json'].decode())
    options = settings.get('o') if isinstance(settings, dict) else None
    if not isinstance(options, dict) or options.get('PortDevices') != {}:
        raise ValueError('Non-default Nymashock port devices are unsupported')
    declared = options.get('MednafenValues')
    if (not isinstance(declared, dict) or any(key not in ALLOWED_MEDNAFEN_KEYS for key in declared) or
            any(value not in ('0', '1') for value in declared.values())):
        raise ValueError('Unsupported Nymashock sync settings')
    lines = parts['Input Log.txt'].decode().splitlines()
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
        if any(not 0 <= value <= 0xFFFF for value in axes):
            raise ValueError('Axis outside 16-bit range')
        rows.append((axes, controller[4]))
    if not rows:
        raise ValueError('Empty controller route')
    return header, options, rows


def convert(movie_bytes, table):
    """Return (PSXRTI2 rows, receipt fields) for a 2.8 movie."""
    header, options, rows = load_movie(movie_bytes)
    converted, raw_axes, pressed = [], hashlib.sha256(), dict.fromkeys(route.LOGKEY_28.split('|')[8:25], 0)
    names = tuple(pressed)
    for (rx, lx, ry, ly), buttons in rows:
        raw_axes.update(b'%d,%d,%d,%d;' % (rx, lx, ry, ly))
        word = 0xFFFF
        for character, bit in zip(buttons[:16], route.BUTTON_BITS):
            if character != '.':
                word &= ~(1 << bit)
        for name, character in zip(names, buttons):
            if character != '.':
                pressed[name] += 1
        # PSXRTI2 keeps LY, LX, RY, RX; the pad byte comes from the table, never from this file.
        converted.append((word, table[ly], table[lx], table[ry], table[rx], int(buttons[16] != '.')))
    return converted, {
        'schema': SCHEMA, 'format': 'PSXRTI2', 'source_layout': 'nymashock-2.8-p1',
        'frames': len(converted), 'header': header,
        'declared_mednafen_values': options.get('MednafenValues'),
        'axis_order': AXIS_ORDER, 'axis_table_sha256': hashlib.sha256(table).hexdigest(),
        'axis_conversion': 'external/source_dualshock_axis.py table lookup (Mednafen InputDevice_DualShock)',
        'raw_axis_sha256': raw_axes.hexdigest(), 'pressed_frame_counts': pressed,
    }


def read_movie_nymashock28(movie, table=None, mednafen_values=None):
    """Return (rows, facts) from a cold BizHawk 2.8 Nymashock movie with a DualShock on P1.

    movie: a path or file object that zipfile opens (convert's parameter, despite its name).

    rows are dualshock_route.read_movie rows: (active-low buttons, LY, LX, RY, RX, physical Analog).
    The four axis bytes are the bytes the pad itself reports, taken from the Mednafen conversion
    table (default: external/source_dualshock_axis.py), so a runtime must deliver them unscaled:
    run_native --axis-model octoshock-byte, whose name is the Octoshock case of the same rule
    (the route byte is the protocol byte), not a claim about the source core. The Nymashock
    default would rescale them a second time.

    The physical Analog button is retained in its own field, never interpreted here; a route that
    presses it is refused downstream until a runtime models the pad's mode toggle.

    table:           the 65,536-byte pad table; None loads the exported one.
    mednafen_values: the sync settings the title's source declares, e.g. {'psx.input.port1.memcard':
                     '0'}; None accepts any value convert() accepts, a dict must match exactly (a
                     movie the caller's source reference was not built for is refused).
    Every refusal of load_movie stays: console events, other ports, anchored movies, a log layout or
    sync setting this encoder does not model."""
    table = default_axis_table() if table is None else check_axis_table(table)
    rows, receipt = convert(movie, table)
    declared = receipt['declared_mednafen_values']
    if mednafen_values is not None and declared != mednafen_values:
        raise ValueError('Movie sync settings differ from the title: %r' % (declared,))
    return rows, {
        'declared_emu_version': EMU_VERSION, 'declared_mednafen_values': declared,
        'port1_memcard': declared.get('psx.input.port1.memcard', 'default'),
        'stick_frames': sum(any(value != 128 for value in row[1:5]) for row in rows),
        'mode_press_frames': sum(row[5] for row in rows),
        'stick_bytes': 'pad bytes from the Nymashock axis table; deliver unscaled',
        'axis_order': AXIS_ORDER, 'axis_table_sha256': receipt['axis_table_sha256'],
        'raw_axis_sha256': receipt['raw_axis_sha256'],
        'runtime_requirement': ('a MODE press needs the pad model that toggles analog mode on the '
                                'button rising edge while DTR is low' if any(row[5] for row in rows) else None)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('movie', type=Path)
    parser.add_argument('--axis-table', type=Path, required=True,
                        help='table written by external/source_dualshock_axis.py')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.receipt.exists():
        parser.error('choose new output and receipt paths')
    table = load_axis_table(args.axis_table)
    rows, receipt = convert(args.movie, table)
    written = route.write_route(rows, args.output)
    receipt.update({'movie_sha256': hashlib.sha256(args.movie.read_bytes()).hexdigest(),
                    'axis_table': str(args.axis_table), 'route': written})
    with args.receipt.open('x', encoding='utf-8') as stream:
        json.dump(receipt, stream, indent=2)
        stream.write('\n')
    print(json.dumps({'frames': receipt['frames'], 'steps': written['steps'],
                      'canonical_controller_sha256': written['canonical_controller_sha256']}))


if __name__ == '__main__':
    main()
