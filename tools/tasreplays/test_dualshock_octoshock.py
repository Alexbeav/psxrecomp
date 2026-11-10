"""dualshock_route.read_movie_octoshock_dualshock: sticks, MODE, card and resaved containers.

Also guards what must NOT change: the strict read_movie_octoshock27 path Crash is qualified on,
and run_native.route_identity's refusal of a physical Analog press, which is the gate that keeps a
Spyro route out of the pipeline until a runtime models the MODE rule.
"""
import io
import json
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dualshock_route as route  # noqa: E402
import run_native  # noqa: E402

OCTO_HEADER = ('MovieVersion BizHawk v2.0\nAuthor t\nCore Octoshock\nPlatform PSX\n'
               'emuVersion Version 2.7.0\nGameName t\nSHA1 ABCD1234\n')
FIO = {'Multitaps': [False, False], 'Memcards': [False, False], 'Devices8': [2, 0, 0, 0, 0, 0, 0, 0]}
SYNC = {'o': {'$type': 'BizHawk.Emulation.Cores.Sony.PSX.Octoshock+SyncSettings, BizHawk.Emulation.Cores',
              'EnableLEC': False, 'FIOConfig': FIO}}
NEUTRAL = '.' * 17


def movie(rows, header=OCTO_HEADER, sync=None, resaved=False, log_key=route.LOGKEY_OCTO27_DUALSHOCK):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        parts = {'Header.txt': header.encode(), 'Comments.txt': b'\r\n', 'Subtitles.txt': b'\r\n',
                 'SyncSettings.json': json.dumps(sync or SYNC).encode(),
                 'Input Log.txt': ('[Input]\r\n' + log_key + '\r\n' +
                                   ''.join(r + '\r\n' for r in rows) + '[/Input]\r\n').encode()}
        if resaved:
            parts['BizState 1.0'] = b'2\r\n'
            parts['BizVersion.txt'] = b'Version 2.9.1\r\n'
        for name, data in parts.items():
            archive.writestr(name, data)
    return buffer.getvalue()


def row(lx=128, ly=128, rx=128, ry=128, buttons=NEUTRAL, console='    1,...'):
    return '|%s|%5d,%5d,%5d,%5d,%s|' % (console, lx, ly, rx, ry, buttons)


def refuses(call, text):
    try:
        call()
    except ValueError as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('expected refusal: ' + text)


# -------------------------------------------------- sticks pass through unscaled, in PSXRTI2 order
rows, facts = route.read_movie_octoshock_dualshock(io.BytesIO(movie([row(lx=10, ly=20, rx=30, ry=40)])))
assert rows[0][1:5] == (20, 10, 40, 30), rows[0]      # LY, LX, RY, RX
assert facts['stick_frames'] == 1 and facts['mode_press_frames'] == 0
assert rows[0][0] == 0xFFFF and rows[0][5] == 0
# Every byte survives exactly; Octoshock does not rescale.
sweep = [row(lx=v, ly=v, rx=v, ry=v) for v in range(256)]
rows, _ = route.read_movie_octoshock_dualshock(io.BytesIO(movie(sweep)))
assert [r[1] for r in rows] == list(range(256))

# -------------------------------------------------- MODE is kept and counted, never interpreted
rows, facts = route.read_movie_octoshock_dualshock(io.BytesIO(movie([row(), row(buttons='.' * 16 + 'A'), row()])))
assert [r[5] for r in rows] == [0, 1, 0] and facts['mode_press_frames'] == 1
assert 'rising edge' in facts['runtime_requirement']
rows, facts = route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()])))
assert facts['runtime_requirement'] is None

# -------------------------------------------------- buttons use the Octoshock bit order
for index, bit in enumerate(route.BUTTON_BITS_OCTO):
    pressed = '.' * index + 'x' + '.' * (16 - index)
    rows, _ = route.read_movie_octoshock_dualshock(io.BytesIO(movie([row(buttons=pressed)])))
    assert rows[0][0] == 0xFFFF & ~(1 << bit), index

# -------------------------------------------------- versions, card and resaved container
rows, facts = route.read_movie_octoshock_dualshock(
    io.BytesIO(movie([row()], header=OCTO_HEADER.replace('2.7.0', '2.3.0'))))
assert facts['declared_emu_version'] == 'Version 2.3.0' and facts['resaved_container'] is False
resaved_header = OCTO_HEADER.replace('emuVersion Version 2.7.0', 'emuVersion Version 2.9.1\nOriginalEmuVersion Version 2.7.0')
rows, facts = route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()], header=resaved_header, resaved=True)),
                                                   allow_resaved=True)
assert facts['resaved_container'] is True and facts['declared_emu_version'] == 'Version 2.7.0'
refuses(lambda: route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()], header=resaved_header, resaved=True))),
        'cold Octoshock input-only movie required')
card_sync = {'o': dict(SYNC['o'], FIOConfig=dict(FIO, Memcards=[True, False]))}
rows, facts = route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()], sync=card_sync)), allow_card=True)
assert facts['memcards'] == [True, False]
refuses(lambda: route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()], sync=card_sync))), 'card layout')

# -------------------------------------------------- refusals
refuses(lambda: route.read_movie_octoshock_dualshock(io.BytesIO(movie([row(console='    2,...')]))), 'Console events')
refuses(lambda: route.read_movie_octoshock_dualshock(io.BytesIO(movie([row(console='    1,O..')]))), 'Console events')
refuses(lambda: route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()], header=OCTO_HEADER.replace('2.7.0', '2.10')))),
        'Octoshock PSX log layouts')
refuses(lambda: route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()], header=OCTO_HEADER + 'StartsFromSavestate True\n'))),
        'Anchored')
refuses(lambda: route.read_movie_octoshock_dualshock(io.BytesIO(movie([row()], log_key=route.LOGKEY_28))), 'input layout')

# -------------------------------------------------- the strict Crash path is unchanged
strict = route.read_movie_octoshock27(io.BytesIO(movie([row(buttons='x' + '.' * 16)])))
assert strict == [(0xFFFF & ~(1 << route.BUTTON_BITS_OCTO[0]), 128, 128, 128, 128, 0)]
for bad in (row(lx=200), row(buttons='.' * 16 + 'A')):
    try:
        route.read_movie_octoshock27(io.BytesIO(movie([bad])))
        raise AssertionError('strict reader must still refuse sticks and MODE')
    except ValueError as error:
        assert 'no exact PSXRTI2 spelling' in str(error)

print('synthetic octoshock dualshock tests passed')

# -------------------------------------------------- real movies and the identity gate
MOVIES = Path(r'D:\psxrecomp\validation\tas\movies')
REAL = [('toastedkat_wafflewizard1-spyrothedragon.bk2', 128104, dict(allow_resaved=True)),
        ('nitrofski_lapogne36-spyroyotd.bk2', 80352, {}),
        ('wafflewizard1_jeremythompson-spyroyotd-100eggs.bk2', 163115, dict(allow_card=True))]
for name, frames, kwargs in REAL:
    path = MOVIES / name
    if not path.is_file():
        print(f'skipped (absent): {path}')
        continue
    rows, facts = route.read_movie_octoshock_dualshock(path, **kwargs)
    assert len(rows) == frames, (name, len(rows))
    assert all(0 <= v <= 255 for r in rows for v in r[1:5]) and all(r[5] in (0, 1) for r in rows)
    print(f'{name}: {frames} frames, {facts["stick_frames"]} stick frames, '
          f'{facts["mode_press_frames"]} MODE presses, cards {facts["memcards"]}, '
          f'resaved {facts["resaved_container"]}')

# A route carrying a MODE press must still be refused by the pipeline: the runtime does not model
# the rule yet, and this refusal is what will require it.
import struct  # noqa: E402
import tempfile  # noqa: E402
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'analog.psxrti2'
    route.write_route([(0xFFFF, 128, 128, 128, 128, 1)], path)
    try:
        run_native.route_identity(path)
        raise AssertionError('route_identity must refuse a physical Analog press')
    except ValueError as error:
        assert 'physical Analog press' in str(error)
    plain = Path(tmp) / 'plain.psxrti2'
    receipt = route.write_route([(0xFFFF, 128, 128, 128, 128, 0)], plain)
    identity = run_native.route_identity(plain)
    assert identity['format'] == 'PSXRTI2' and identity['frames'] == 1 and identity['steps'] == 1

# Every staged qualified route must still report the identity recorded with it.
STAGED = Path(r'Z:\Share\psxrecomp\tas-evidence\qualified-routes-20260917')
expected = STAGED / 'expected-route-identity.json'
if expected.is_file():
    recorded = json.loads(expected.read_text())['results']
    for name, fields in sorted(recorded.items()):
        actual = run_native.route_identity(STAGED / name)
        assert actual == fields, name
    print(f'{len(recorded)} qualified route identities unchanged')
else:
    print(f'skipped (absent): {expected}')
print('octoshock dualshock tests passed')
