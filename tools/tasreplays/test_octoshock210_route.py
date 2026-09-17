"""octoshock210_route.py: BizHawk 2.10 row reading, tray/disc event model, PSXRTI3 layout.

Synthetic movies cover the wrapper rules; the final section converts the real Abe's Exoddus
6672M movie in memory when it is present on this machine.
"""
import io
import json
import struct
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import octoshock210_route as o  # noqa: E402

D1, D2 = '11' * 32, '22' * 32
HEADER = 'MovieVersion BizHawk v2.0.0\nCore Octoshock\nPlatform PSX\nemuVersion Version 2.10\nGameName t\n'


def movie(rows, header=HEADER, sync=None, members=None, log_key=o.LOG_KEY):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        parts = {'BizState 1.0': b'2\r\n', 'BizVersion.txt': b'Version 2.10\r\n', 'Header.txt': header.encode(),
                 'Comments.txt': b'\r\n', 'Subtitles.txt': b'\r\n',
                 'SyncSettings.json': json.dumps(sync or o.SYNC).encode(),
                 'Input Log.txt': ('[Input]\r\n' + log_key + '\r\n' + ''.join(r + '\r\n' for r in rows) + '[/Input]\r\n').encode()}
        for name, data in (members or parts).items():
            archive.writestr(name, data)
    return buffer.getvalue()


def row(select=1, console='...', p1='.' * 14, p2='.' * 14):
    return f'|{select:5d},{console}|{p1}|{p2}|'


def refuses(call, text):
    try:
        call()
    except ValueError as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('expected refusal: ' + text)


# ------------------------------------------------------------- row reading
controls = o.controls_from_log_key(o.LOG_KEY)
assert len(controls) == 4 + 28 and controls[:5] == ['Disc Select', 'Open', 'Close', 'Reset', 'P1 Up']
values, rest = o.parse_row('||    2,...|...R.....X.r..|..............|', controls)
assert values['Disc Select'] == 2 and values['P1 Right'] and values['P1 Cross'] and values['P1 R1'] and rest == '|'
values, rest = o.parse_row('|    2,...|...R..........|..............||    2,...|U.............|..............|', controls)
assert values['P1 Right'] and not values['P1 Up'] and rest.startswith('||'), 'a second appended frame is ignored'
values, rest = o.parse_row('|    1,...|.........Xl...|..............', controls)
assert values['P1 Cross'] and values['P1 L1'] and rest == ''
refuses(lambda: o.parse_row('|    1,...|....', controls), 'ends before')
refuses(lambda: o.parse_row('|  x1,...|' + '.' * 28, controls), 'unparseable axis')

# ------------------------------------------------------------- events and layout
rows = [row(console='.C.')] + [row() for _ in range(4)]
rows += [row(console='O..', p1='U' + '.' * 13)]          # frame 5: open
rows += [row(select=1, console='O..')]                   # frame 6: Open while open -> nothing
rows += [row(select=2)]                                  # frame 7: select 2 while open -> DISC_SELECT
rows += [row(select=2, console='.C.', p2='.' * 9 + 'X' + '.' * 4)]  # frame 8: close on disc 2
rows += [row(select=1)]                                  # frame 9: select 1 while closed -> nothing
rows += [row(select=1, console='..r', p1='.' * 5 + 'S' + '.' * 8)]  # frame 10: reset
data, receipt = o.convert(movie(rows), [D1, D2])
assert receipt['console_events'] == [
    {'frame': 5, 'event': 'TRAY_OPEN', 'disc_index': 0}, {'frame': 7, 'event': 'DISC_SELECT', 'disc_index': 2},
    {'frame': 8, 'event': 'TRAY_CLOSE', 'disc_index': 2}, {'frame': 10, 'event': 'RESET', 'disc_index': 0}]
assert receipt['irregular_rows'] == [] and receipt['frame_count'] == 11 and receipt['p2_pressed_frames'] == 1
magic, version, size, count, flags, ext = struct.unpack_from('<8sIIIII', data)
assert (magic, version, size, count, flags) == (b'PSXRTI3\0', 3, 12, 11, 0) and ext % 4 == 0
assert len(data) == 28 + ext + 12 * 11
back = o.read_psxrti3(data)
assert back['events'] == [(5, 2, 0), (7, 4, 2), (8, 3, 2), (10, 1, 0)] and back['discs'] == [(1, D1), (1, D2)]
assert back['rows'][0] == (0xFFFF, 0xFFFF) and back['rows'][5] == (0xFFFF & ~(1 << 4), 0xFFFF)
assert back['rows'][8] == (0xFFFF, 0xFFFF & ~(1 << 14)) and back['rows'][10] == (0xFFFF & ~(1 << 3), 0xFFFF)
# First TLV is the port layout, 6 bytes padded to 8.
assert struct.unpack_from('<II', data, 28) == (0x201, 6) and data[36:44] == bytes([2, 0, 0, 0, 1, 1, 0, 0])

# Every button maps to its SetInput bit, alone.
for index, (name, bit) in enumerate(zip(o.BUTTONS, o.BITS)):
    p1 = '.' * index + 'x' + '.' * (13 - index)
    _, r = o.convert(movie([row(), row(p1=p1)]), [D1])
    assert o.read_psxrti3(_)['rows'][1] == (0xFFFF & ~(1 << bit), 0xFFFF), name
assert sorted(o.BITS) == [0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]

# ------------------------------------------------------------- refusals
refuses(lambda: o.convert(movie([row(console='O..')]), [D1]), 'frame 0')
refuses(lambda: o.convert(movie([row(select=2)]), [D1, D2]), 'frame 0')
refuses(lambda: o.convert(movie([row(), row(console='O..'), row(select=3), row(select=3, console='.C.')]), [D1, D2]),
        'outside the disc set')
refuses(lambda: o.convert(movie([row(), row(console='O..')]), [D1]), 'tray open')
refuses(lambda: o.convert(movie([row()], header=HEADER.replace('2.10', '2.7.0')), [D1]), 'emuVersion')
refuses(lambda: o.convert(movie([row()], sync={'o': dict(o.SYNC['o'], EnableLEC=True)}), [D1]), 'synchronization')
refuses(lambda: o.convert(movie([row()], log_key=o.LOG_KEY.replace('#P2 Up', '#P2 Down', 1)), [D1]), 'LogKey')
refuses(lambda: o.convert(movie([row()]), ['11' * 31]), 'digest')
refuses(lambda: o.convert(movie([row()], header=HEADER + 'StartsFromSavestate True\n'), [D1]), 'anchored')

# ------------------------------------------------------------- strict reader
good, _ = o.convert(movie(rows), [D1, D2])
refuses(lambda: o.read_psxrti3(good + b'\0'), 'file size')
refuses(lambda: o.read_psxrti3(good[:20] + struct.pack('<I', 1) + good[24:]), 'header identity')
bad = bytearray(good)
struct.pack_into('<I', bad, 28 + 8 + 8, 0x999)  # second TLV tag -> unknown mandatory
refuses(lambda: o.read_psxrti3(bytes(bad)), 'unknown mandatory')
bad = bytearray(good)
bad[28 + 8 + 6] = 1  # padding byte of the port-layout TLV
refuses(lambda: o.read_psxrti3(bytes(bad)), 'malformed extension')
bad = bytearray(good)
ext = struct.unpack_from('<I', good, 24)[0]
struct.pack_into('<I', bad, 28 + ext + 12 * 3, 99)  # record sequence
refuses(lambda: o.read_psxrti3(bytes(bad)), 'record sequence')
# record_size does not identify the layout: this two-digital-pad record and a PSXRTI2 DualShock
# record are both 12 bytes. Without the port-layout tag the reader must refuse, never guess.
bare = o.HEADER.pack(o.MAGIC, 3, 12, 1, 0, 0) + struct.pack('<IHHHH', 1, 0xFFFF, 0, 0xFFFF, 0)
refuses(lambda: o.read_psxrti3(bare), 'unsupported port layout')
# A layout naming a DualShock (a record this tool does not write) is refused rather than misread.
dualshock = struct.pack('<II', o.TAG_PORT_LAYOUT, 6) + bytes([1, 0, 0, 0, o.DEVICE_DUALSHOCK, 0]) + b'\0\0'
sized = o.HEADER.pack(o.MAGIC, 3, 12, 1, 0, len(dualshock)) + dualshock + struct.pack('<IHHHH', 1, 0xFFFF, 0, 0xFFFF, 0)
refuses(lambda: o.read_psxrti3(sized), 'unsupported port layout')

# A skippable (bit 31) tag the reader does not know is accepted.
extra = struct.pack('<II', 0x80000101, 4) + b'abcd'
skippable = good[:24] + struct.pack('<I', ext + len(extra)) + good[28:28 + ext] + extra + good[28 + ext:]
assert o.read_psxrti3(skippable)['events'] == back['events']

print('synthetic octoshock210_route tests passed')

# ------------------------------------------------------------- real movie
REAL = Path(r'D:\psxrecomp\validation\tas\movies\samtastic-oddworldabesexoddus-100p.bk2')
if REAL.is_file():
    data, receipt = o.convert(REAL.read_bytes(), [D1, D2])
    assert receipt['frame_count'] == 478752
    assert receipt['console_events'] == [
        {'frame': 179993, 'event': 'TRAY_OPEN', 'disc_index': 0},
        {'frame': 179996, 'event': 'DISC_SELECT', 'disc_index': 2},
        {'frame': 179996, 'event': 'TRAY_CLOSE', 'disc_index': 2}], receipt['console_events']
    assert [r['frame'] for r in receipt['irregular_rows']] == [190887, 297572, 370534, 414738, 425431, 470648]
    assert receipt['p2_pressed_frames'] == 0
    assert len(o.read_psxrti3(data)['rows']) == 478752
    print('real Abe\'s Exoddus 6672M movie converted:', receipt['output_sha256'])
else:
    print(f'skipped real movie: {REAL} not present')
print('octoshock210_route tests passed')
