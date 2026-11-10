"""nymashock28_route.py: 2.8 axis order, the pad conversion by table, refusals, real movies.

The conversion itself is not asserted here beyond its endpoints; it is data produced by
external/source_dualshock_axis.py, and this checks that the encoder applies it in the right
axis order and never invents a byte of its own.
"""
import io
import json
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dualshock_route as route  # noqa: E402
import nymashock28_route as n  # noqa: E402

sys.path.insert(0, str(HERE / 'external'))
import source_dualshock_axis as axis_tool  # noqa: E402

TABLE = axis_tool.table()
HEADER = ('MovieVersion BizHawk v2.0.0\nCore Nymashock\nPlatform PSX\nemuVersion Version 2.8\n'
          'GameName t\nSHA1 ABCD1234\n')
SYNC = {'o': {'$type': 'BizHawk.Emulation.Cores.Waterbox.NymaCore+NymaSyncSettings, BizHawk.Emulation.Cores',
              'MednafenValues': {}, 'PortDevices': {}}}
NEUTRAL = '.' * 17


def movie(rows, header=HEADER, sync=None, log_key=route.LOGKEY_28, members=None):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        parts = {'Header.txt': header.encode(), 'Comments.txt': b'\r\n', 'Subtitles.txt': b'\r\n',
                 'SyncSettings.json': json.dumps(sync or SYNC).encode(),
                 'Input Log.txt': ('[Input]\r\n' + log_key + '\r\n' +
                                   ''.join(r + '\r\n' for r in rows) + '[/Input]\r\n').encode()}
        for name, data in (members or parts).items():
            archive.writestr(name, data)
    return buffer.getvalue()


def row(rx=0x8000, lx=0x8000, ry=0x8000, ly=0x8000, buttons=NEUTRAL, console='....'):
    return '|%s|%5d,%5d,%5d,%5d,%s|' % (console, rx, lx, ry, ly, buttons)


def refuses(call, text):
    try:
        call()
    except ValueError as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('expected refusal: ' + text)


# ------------------------------------------------------------------ axis order
# Distinct values per axis prove the 2.8 order (RX, LX, RY, LY) lands as PSXRTI2 (LY, LX, RY, RX).
rows, receipt = n.convert(io.BytesIO(movie([row(rx=0x1000, lx=0x2000, ry=0x3000, ly=0x4000)])), TABLE)
assert rows[0][1:5] == (TABLE[0x4000], TABLE[0x2000], TABLE[0x3000], TABLE[0x1000])
assert receipt['axis_order'] == n.AXIS_ORDER and receipt['frames'] == 1
assert rows[0][0] == 0xFFFF and rows[0][5] == 0

# Every byte in a route must come from the table, for every possible axis value.
sweep = [row(rx=v, lx=v, ry=v, ly=v) for v in range(0, 0x10000, 97)]
rows, _ = n.convert(io.BytesIO(movie(sweep)), TABLE)
assert all(r[1] == r[2] == r[3] == r[4] == TABLE[v] for r, v in zip(rows, range(0, 0x10000, 97)))
# Neutral, and the values the published movies actually carry, survive unchanged.
assert TABLE[0x8000] == 128 and TABLE[0xFFFF] == 255 and TABLE[0] == 0

# ------------------------------------------------------------------ buttons
for index, bit in enumerate(route.BUTTON_BITS):
    pressed = '.' * index + 'x' + '.' * (16 - index)
    rows, _ = n.convert(io.BytesIO(movie([row(buttons=pressed)])), TABLE)
    assert rows[0][0] == 0xFFFF & ~(1 << bit), index
# The seventeenth character is the physical Analog button, kept as its own field.
rows, _ = n.convert(io.BytesIO(movie([row(buttons='.' * 16 + 'A')])), TABLE)
assert rows[0][0] == 0xFFFF and rows[0][5] == 1

# ------------------------------------------------------------------ sync settings
card_off = {'o': dict(SYNC['o'], MednafenValues={'psx.input.port1.memcard': '0'})}
rows, receipt = n.convert(io.BytesIO(movie([row()], sync=card_off)), TABLE)
assert receipt['declared_mednafen_values'] == {'psx.input.port1.memcard': '0'}
refuses(lambda: n.convert(io.BytesIO(movie([row()], sync={'o': dict(SYNC['o'], PortDevices={'0': 'gamepad'})})), TABLE),
        'port devices')
refuses(lambda: n.convert(io.BytesIO(movie([row()], sync={'o': dict(SYNC['o'], MednafenValues={'psx.dbg_level': '1'})})), TABLE),
        'sync settings')

# ------------------------------------------------------------------ refusals
refuses(lambda: n.convert(io.BytesIO(movie([row(console='P...')])), TABLE), 'Console events')
refuses(lambda: n.convert(io.BytesIO(movie([row()], header=HEADER.replace('2.8', '2.9.1'))), TABLE), '2.8 PSX log layout')
refuses(lambda: n.convert(io.BytesIO(movie([row()], header=HEADER + 'StartsFromSavestate True\n')), TABLE), 'Anchored')
refuses(lambda: n.convert(io.BytesIO(movie([row()], log_key=route.LOGKEY)), TABLE), 'input layout')
# An empty log fails the framing check before the row loop, which is the stricter refusal.
refuses(lambda: n.convert(io.BytesIO(movie([])), TABLE), 'input layout')
refuses(lambda: n.load_axis_table(__file__), 'table must be')
with tempfile.TemporaryDirectory() as tmp:
    bad = Path(tmp) / 'bad.bin'
    bad.write_bytes(bytes(n.TABLE_BYTES))
    refuses(lambda: n.load_axis_table(bad), 'endpoints')

print('synthetic nymashock28_route tests passed')

# ------------------------------------------------------------------ real movies
MOVIES = Path(r'D:\psxrecomp\validation\tas\movies')
for name, frames in (('crash41596-medievil-allchalices.bk2', 222294),
                     ('lapogne36-spyro2-moneybags.bk2', 86655)):
    path = MOVIES / name
    if not path.is_file():
        print(f'skipped (absent): {path}')
        continue
    rows, receipt = n.convert(io.BytesIO(path.read_bytes()), TABLE)
    assert receipt['frames'] == frames == len(rows), name
    assert all(0 <= value <= 255 for r in rows for value in r[1:5]), name
    with tempfile.TemporaryDirectory() as tmp:
        written = route.write_route(rows, Path(tmp) / 'input.psxrti2')
    assert written['steps'] <= route.MAX_STEPS, (name, written['steps'])
    print(f'{name}: {receipt["frames"]} frames, {written["steps"]} steps, '
          f'controller {written["canonical_controller_sha256"][:16]}, '
          f'declared {receipt["declared_mednafen_values"]}')
print('nymashock28_route tests passed')
