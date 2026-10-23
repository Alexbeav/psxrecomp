"""nymashock28_digital_route.py: the plain-gamepad 2.8 reader, its refusals and the real Driver 2 movie.

The real movie is checked only where it is present: PSX_CHICAGO_MOVIE names it, or the private
library holds it (Pegasus). A host without it skips that part and says so.
"""
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import nymashock28_digital_route as n  # noqa: E402

HEADER = ('MovieVersion BizHawk v2.0\nAuthor x\nemuVersion Version 2.8\nOriginalEmuVersion Version 2.8\n'
          'Platform PSX\nGameName t\nCore Nymashock\nrerecordCount 1\n')
SYNC = {'o': {'$type': 'BizHawk.Emulation.Cores.Waterbox.NymaCore+NymaSyncSettings, BizHawk.Emulation.Cores',
              'MednafenValues': {}, 'PortDevices': {'0': 'gamepad'}}}
NEUTRAL = '.' * 14
DOWN, LEFT, CROSS = '.D............', '..L...........', '.......X......'


def movie(rows, header=HEADER, sync=None, log_key=n.LOGKEY, members=None):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        parts = {'Header.txt': header.encode(), 'Comments.txt': b'\r\n', 'Subtitles.txt': b'\r\n',
                 'SyncSettings.json': json.dumps(sync or SYNC).encode(),
                 'Input Log.txt': ('[Input]\r\n' + log_key + '\r\n' +
                                   ''.join(r + '\r\n' for r in rows) + '[/Input]\r\n').encode()}
        for name, data in (members or parts).items():
            archive.writestr(name, data)
    return io.BytesIO(buffer.getvalue())


def row(buttons=NEUTRAL, console='....'):
    return '|%s|%s|' % (console, buttons)


def refuses(call, text):
    try:
        call()
    except ValueError as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('expected refusal: ' + text)


# Active-low words by the shared bit table: Down is bit 6, Left bit 7, X bit 14.
words = n.read_movie(movie([row(NEUTRAL), row(DOWN), row(LEFT), row(CROSS), row(NEUTRAL)]))
assert words == [0xFFFF, 0xFFFF & ~(1 << 6), 0xFFFF & ~(1 << 7), 0xFFFF & ~(1 << 14), 0xFFFF], words
assert n.BUTTON_BITS == (4, 6, 7, 5, 0, 3, 12, 14, 15, 13, 10, 8, 11, 9)

# The encoder: header, records and the hash of the active-low words.
data, receipt = n.encode(words)
assert data[:8] == b'PSXRTI1\0' and struct.unpack_from('<IIII', data, 8) == (1, 8, 5, 0)
assert len(data) == 24 + 8 * 5 and struct.unpack_from('<IHH', data, 24 + 8 * 2) == (3, words[2], 0)
assert receipt['frames'] == 5 and receipt['steps'] == 5
assert receipt['words_sha256'] == hashlib.sha256(b''.join(struct.pack('<H', w) for w in words)).hexdigest()

# Everything the reader does not understand is refused.
refuses(lambda: n.read_movie(movie([row()], header=HEADER.replace('Version 2.8', 'Version 2.9.1'))), 'Nymashock2.8')
refuses(lambda: n.read_movie(movie([row()], header=HEADER.replace('Nymashock', 'Octoshock'))), 'Nymashock2.8')
refuses(lambda: n.read_movie(movie([row()], header=HEADER + 'StartsFromSavestate 1\n')), 'Anchored')
other = json.loads(json.dumps(SYNC))
other['o']['PortDevices'] = {'0': 'dualshock'}
refuses(lambda: n.read_movie(movie([row()], sync=other)), 'plain gamepad')
other['o']['PortDevices'] = {}
refuses(lambda: n.read_movie(movie([row()], sync=other)), 'plain gamepad')
other['o']['PortDevices'] = {'0': 'gamepad', '1': 'gamepad'}
refuses(lambda: n.read_movie(movie([row()], sync=other)), 'plain gamepad')
other['o'].update(PortDevices={'0': 'gamepad'}, MednafenValues={'x': '1'})
refuses(lambda: n.read_movie(movie([row()], sync=other)), 'plain gamepad')
refuses(lambda: n.read_movie(movie([row()], log_key=n.LOGKEY.replace('P1 Up', 'P1 Down'))), 'layout')
refuses(lambda: n.read_movie(movie([row(NEUTRAL, console='.R..')])), 'Console events')
refuses(lambda: n.read_movie(movie(['|....|%s|%s|' % (NEUTRAL, NEUTRAL)])), 'Console events')
refuses(lambda: n.read_movie(movie([row(NEUTRAL[:-1])])), 'Malformed')
refuses(lambda: n.read_movie(movie([row(NEUTRAL + '.')])), 'Malformed')
refuses(lambda: n.read_movie(movie([row('.|............')])), 'Console events')
refuses(lambda: n.read_movie(movie([row(DOWN), row('.d............')])), 'two ways')
refuses(lambda: n.read_movie(movie([row('. ............')])), 'Unexpected')
refuses(lambda: n.read_movie(movie([])), 'layout')
refuses(lambda: n.read_movie(movie([row()], members={'Header.txt': HEADER.encode()})), 'payload')
refuses(lambda: n.encode([]), 'capacity')
refuses(lambda: n.encode([0x10000]), 'Invalid')
refuses(lambda: n.encode([0xFFFF, 0xFFFE] * (n.MAX_STEPS // 2 + 1)), 'Step capacity')

# export never replaces an existing route.
with tempfile.TemporaryDirectory(prefix='nyma28-digital-') as folder:
    output = Path(folder) / 'route.bin'
    movie_path = Path(folder) / 'm.bk2'
    movie_path.write_bytes(movie([row(DOWN), row(NEUTRAL)]).getvalue())
    receipt = n.export(movie_path, output)
    assert output.read_bytes()[:8] == b'PSXRTI1\0' and receipt['output_sha256'] == hashlib.sha256(output.read_bytes()).hexdigest()
    try:
        n.export(movie_path, output)
    except FileExistsError:
        pass
    else:
        raise AssertionError('export replaced an existing route')

# The real Driver 2 Chicago movie (4726M), where present.
REAL_SHA256 = '7e51bcb4'
LIBRARY = Path(r'D:\Projects\Recompilations\PS1\Library\PS1B-109\intake\movies')
named = os.environ.get('PSX_CHICAGO_MOVIE', '')
if named and not Path(named).is_file():
    raise AssertionError('PSX_CHICAGO_MOVIE names no file: ' + named)
real = Path(named) if named else LIBRARY / 'thunderaxe31-driver2-survival,chicago.bk2'
if not real.is_file():
    print('real-movie check SKIPPED: the Driver 2 Chicago movie (4726M) is not on this host '
          '(set PSX_CHICAGO_MOVIE to its .bk2 to run it)')
else:
    assert hashlib.sha256(real.read_bytes()).hexdigest().startswith(REAL_SHA256)
    chicago = n.read_movie(real)
    assert len(chicago) == 8034 and chicago[0] == 0xFFFF
    data, receipt = n.encode(chicago)
    assert receipt['frames'] == 8034 and len(data) == 24 + 8 * 8034
    print('real Chicago movie: 8034 rows, %d steps, words_sha256 %s' % (receipt['steps'], receipt['words_sha256']))
print('nymashock28_digital_route: word table, header and record layout, 22 refusals, no-replace export PASS')
