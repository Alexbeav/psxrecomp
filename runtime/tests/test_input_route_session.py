"""Record -> replay round trip and release gate for input_route_session.c.

usage: test_input_route_session.py DIAGNOSTIC_EXE RELEASE_EXE

DIAGNOSTIC_EXE and RELEASE_EXE are test_input_route_session.c built without
and with PSX_NO_DEBUG_TOOLS. Authored discs and routes only.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import input_route_v3  # noqa: E402

diagnostic, release = (Path(p).resolve() for p in sys.argv[1:3])
checks = 0


def check(ok, what, detail=''):
    global checks
    if not ok:
        print(f'FAIL: {what}\n{detail}', file=sys.stderr)
        sys.exit(1)
    checks += 1


def run(exe, *args, markers_exit=True, extra_env=None):
    env = dict(os.environ)
    env.update(extra_env or {})
    env.pop('PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS', None)
    if markers_exit:
        env['PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS'] = '1'
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True,
                            env=env, timeout=120)
    return result.returncode, result.stdout, result.stderr


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    # A two-track cue disc and its BIOS.
    disc = root / 'disc'
    disc.mkdir()
    (disc / 'Game (Track 1).bin').write_bytes(bytes(range(256)) * 64)
    (disc / 'Game (Track 2).bin').write_bytes(b'\x5a' * 7056)
    cue = disc / 'Game.cue'
    cue.write_bytes(b'\xef\xbb\xbfFILE "Game (Track 1).bin" BINARY\r\n  TRACK 01 MODE2/2352\r\n'
                    b'    INDEX 01 00:00:00\r\nfile "Game (Track 2).bin" binary\r\n'
                    b'  TRACK 02 AUDIO\r\n    INDEX 01 00:00:00\r\n')
    bios = root / 'SCPH1001.BIN'
    bios.write_bytes(b'\0' * 16)

    # Record on the diagnostic product.
    route = root / 'pe.psxrti3'
    code, out, err = run(diagnostic, 'record', route, cue, bios, 200, markers_exit=False)
    check(code == 0 and route.is_file(), 'record exits cleanly and writes the route', out + err)
    check('input_route_recorded: path=' in out and 'frames=200' in out and 'markers=2' in out,
          'recorder reports the written route', out)
    code, out, err = run(diagnostic, 'record', route, cue, bios, 200, markers_exit=False)
    check(code == 10 and 'already exists' in err, 'recorder never overwrites a route', err)

    # The file reads back independently in Python, with the cue disc identity
    # computed the way the TAS lane's checkpoint_asset_digest does.
    parsed = input_route_v3.read(route)
    identity = parsed['identity']
    check(parsed['frames'] == 200 and parsed['record_size'] == 8, 'route shape', parsed)
    check([m['kind'] for m in parsed['markers']] == ['menu', 'gameplay'] and
          [m['frame'] for m in parsed['markers']] == [40, 119], 'marker frames', parsed['markers'])
    check([c['frame'] for c in parsed['checkpoints']] == [40, 119], 'checkpoints at markers')
    check(identity['disc_serial'] == 'SLUS-00662' and identity['bios_stem'] == 'SCPH1001' and
          identity['boot_mode'] == 'hle', 'identity text', identity)
    kind, digest = input_route_v3.disc_digest(cue)
    check((identity['disc_digest_kind'], identity['disc_digest']) == (kind, digest) and kind == 'cue',
          'runtime cue digest equals the Python definition', f'{identity} {kind} {digest}')
    track_hashes = [hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (cue, disc / 'Game (Track 1).bin', disc / 'Game (Track 2).bin')]
    check(digest == hashlib.sha256('\n'.join(track_hashes).encode()).hexdigest(),
          'cue digest joins cue and track digests without a trailing newline')
    try:
        sys.path.insert(0, str(ROOT / 'tools' / 'tasreplays'))
        from run_native import checkpoint_asset_digest
    except ImportError:
        checkpoint_asset_digest = None
    if checkpoint_asset_digest:
        check(checkpoint_asset_digest(cue) == digest, 'digest equals run_native.checkpoint_asset_digest')

    # A route never trusts the player-replay digest cache (path, size, mtime
    # and a head/tail spot hash): with a forged cache for this disc, the
    # recorded identity still carries the full-hash digest.
    forged = root / 'disc_digests.tsv'
    lines = []
    for p in (cue, disc / 'Game (Track 1).bin', disc / 'Game (Track 2).bin'):
        data = p.read_bytes()
        spot = hashlib.sha256(data[:65536] + (data[-65536:] if len(data) > 65536 else b'')).hexdigest()[:16]
        lines.append(f'{len(data)}\t{int(p.stat().st_mtime)}\t{spot}\t{"f" * 64}\t{p}\n')
    forged.write_text(''.join(lines))
    route_cached = root / 'cached.psxrti3'
    code, out, err = run(diagnostic, 'record', route_cached, cue, bios, 200, markers_exit=False,
                         extra_env={'PSX_TEST_DISC_DIGEST_CACHE': str(forged)})
    check(code == 0, 'record with a forged digest cache configured', out + err)
    cached_identity = input_route_v3.read(route_cached)['identity']
    check(cached_identity['disc_digest'] == digest,
          'route identity uses a full disc hash, not the digest cache', cached_identity)

    # Replay on the release product: identity matches, both checkpoints match.
    code, out, err = run(release, 'replay', route, cue, bios)
    check(code == 0, 'release replay reaches the markers and exits 0', out + err)
    check('input_route_identity: match' in out, 'identity match reported', out)
    check(out.count('checkpoint=match') == 2 and 'input_route_markers_complete: markers=2 result=match' in out,
          'both checkpoints match', out)
    gameplay = [line for line in out.splitlines() if 'kind=gameplay' in line][0]
    check(f"ram_sha256={parsed['checkpoints'][1]['ram_sha256']}" in gameplay, 'printed RAM hash equals recorded')

    # A different guest history between the markers is reported, page-located.
    code, out, err = run(release, 'replay', route, cue, bios, 7)
    check(code == 3, 'divergence exits 3', out + err)
    check('kind=menu' in out and out.count('checkpoint=match') == 1 and 'checkpoint=mismatch' in out and
          'differing_pages=0x' in out, 'mismatch names the differing pages', out)

    # Identity refusals: pin, disc bytes, BIOS stem.
    data = route.read_bytes()
    pin = identity['pin'].encode()
    other = bytes(b'0123456789abcdef'[(b'0123456789abcdef'.index(c) + 1) % 16] for c in pin)
    wrong_pin = root / 'wrong-pin.psxrti3'
    wrong_pin.write_bytes(data.replace(pin, other, 1))
    code, out, err = run(release, 'replay', wrong_pin, cue, bios)
    check(code == 21 and 'input route refused' in err and 'pin' in err and other.decode() in err,
          'mismatched pin is refused with both pins', err)
    track = disc / 'Game (Track 2).bin'
    original = track.read_bytes()
    track.write_bytes(original[:-1] + b'\x5b')
    code, out, err = run(release, 'replay', route, cue, bios)
    check(code == 21 and 'disc hash' in err, 'mismatched disc bytes are refused', err)
    track.write_bytes(original)
    bin_disc = disc / 'Game (Track 1).bin'
    code, out, err = run(release, 'replay', route, bin_disc, bios)
    check(code == 21 and 'cue image' in err and 'file image' in err,
          'a different disc container is refused by digest kind', err)
    other_bios = root / 'scph5501.bin'
    other_bios.write_bytes(b'\0' * 16)
    code, out, err = run(release, 'replay', route, cue, other_bios)
    check(code == 21 and 'bios' in err, 'mismatched BIOS stem is refused', err)
    lower_bios = root / 'scph1001.bin'
    lower_bios.write_bytes(b'\0' * 16)
    code, out, err = run(release, 'replay', route, cue, lower_bios)
    check(code == 0, 'BIOS stem compares without case', out + err)

    # Without identity a PSXRTI3 route replays without checks (TAS lane rule).
    bare = root / 'bare.psxrti3'
    ext = 0
    header = struct.pack('<8sIIIII', b'PSXRTI3\0', 3, 8, 3, 0, ext)
    bare.write_bytes(header + b''.join(struct.pack('<IHH', i + 1, w, 0)
                                       for i, w in enumerate((0xfff7, 0xfff7, 0xbfff))))
    code, out, err = run(release, 'words', bare, 5, markers_exit=False)
    check(code == 0 and out.splitlines()[-1] == 'fff7,fff7,bfff,ffff,ffff', 'identity-free v3 replays', out + err)

    # Release replays the frozen formats as they are.
    legacy = root / 'legacy.psxrti'
    words = (0xffff, 0xffef, 0xffef, 0xfff7)
    legacy.write_bytes(struct.pack('<8sIIII', b'PSXRTI1\0', 1, 8, len(words), 0) +
                       b''.join(struct.pack('<IHH', i + 1, w, 0) for i, w in enumerate(words)))
    code, out, err = run(release, 'words', legacy, 6, markers_exit=False)
    check(code == 0 and out.splitlines()[-1] == 'ffff,ffef,ffef,fff7,ffff,ffff',
          'PSXRTI1 release replay holds released buttons after the route', out + err)
    dual = root / 'legacy.psxrti2'
    rows = [(0xffbf, 128, 128, 128, 128), (0xffff, 0, 255, 128, 128)]
    dual.write_bytes(struct.pack('<8sIIII', b'PSXRTI2\0', 2, 12, 2, 0) +
                     b''.join(struct.pack('<IH6B', i + 1, b, *axes, 0, 0) for i, (b, *axes) in enumerate(rows)))
    code, out, err = run(release, 'words', dual, 3, markers_exit=False)
    check(code == 0 and out.splitlines()[-1] == 'ffbf,ffff,ffff', 'PSXRTI2 release replay', out + err)

    # Every admitted route format reports armed (Fast Loading stays off under
    # any route), on both products; nothing admitted is not armed.
    for exe in (diagnostic, release):
        for name, path in (('PSXRTI1', legacy), ('PSXRTI2 DualShock', dual), ('PSXRTI3', route)):
            code, out, err = run(exe, 'armed', path, markers_exit=False)
            check(code == 0 and 'before=0 admitted=1 armed=1' in out,
                  f'{name} route is armed on {exe.name}', out + err)
    broken = root / 'broken.psxrti'
    broken.write_bytes(legacy.read_bytes()[:-1])
    code, out, err = run(release, 'words', broken, 1, markers_exit=False)
    check(code == 30 and 'input route rejected' in err, 'malformed route refused before replay', err)

    # Route pictures on a release product (PS1B-404). With
    # PSX_INPUT_ROUTE_CAPTURE_DIR set, the release replay drives the same
    # observer as the diagnostic product: a picture at every interval and at
    # the route's end, where the process exits with status 0.
    def pictures(path, frames, capture=None, fault='', **more):
        env = {k: v for k, v in os.environ.items() if not k.startswith('PSX_')}
        env.update(capture or {})
        env.update(more)
        result = subprocess.run([str(release), 'pictures', str(path), str(frames), fault],
                                capture_output=True, text=True, env=env, timeout=120)
        return result.returncode, result.stdout, result.stderr

    def png_rows(path):
        data = path.read_bytes()
        check(data[:8] == b'\x89PNG\r\n\x1a\n', f'{path.name} is a PNG')
        at, size, packed = 8, None, b''
        while at < len(data):
            length, kind = struct.unpack('>I4s', data[at:at + 8])
            body = data[at + 8:at + 8 + length]
            if kind == b'IHDR':
                size = struct.unpack('>II', body[:8])
            if kind == b'IDAT':
                packed += body
            at += 12 + length
        raw = zlib.decompress(packed)
        stride = 1 + size[0] * 3
        check(len(raw) == stride * size[1] and all(raw[y * stride] == 0 for y in range(size[1])),
              f'{path.name} holds unfiltered rows')
        return size, [[tuple(raw[y * stride + 1 + x * 3:y * stride + 4 + x * 3])
                       for x in range(size[0])] for y in range(size[1])]

    def words_sha(sequence):
        return hashlib.sha256(b''.join(struct.pack('<H', w) for w in sequence)).hexdigest()

    words = (0xffff, 0xffef, 0xffef, 0xfff7, 0xffff, 0xbfff, 0xffff)
    seven = root / 'seven.psxrti'
    seven.write_bytes(struct.pack('<8sIIII', b'PSXRTI1\0', 1, 8, len(words), 0) +
                      b''.join(struct.pack('<IHH', i + 1, w, 0) for i, w in enumerate(words)))

    def shots(name):
        folder = root / name
        folder.mkdir()
        return folder, {'PSX_INPUT_ROUTE_CAPTURE_DIR': str(folder)}

    # Neither variable, or the interval alone: the replay runs on and writes nothing.
    code, out, err = pictures(seven, 20)
    check(code == 8 and 'still here' in out and 'input_route_capture' not in out and
          'exit_origin' not in out,
          'release replay without a capture directory is unchanged', out + err)
    code, out, err = pictures(seven, 20, {'PSX_INPUT_ROUTE_CAPTURE_EVERY': '3'})
    check(code == 8 and 'still here' in out and 'input_route_capture' not in out,
          'the capture interval alone captures nothing', out + err)

    folder, capture = shots('pictures')
    capture['PSX_INPUT_ROUTE_CAPTURE_EVERY'] = '3'
    code, out, err = pictures(seven, 20, capture)
    check(code == 0 and 'input_route_capture: product=release frames=7' in out and
          'input_route_complete: frames=7' in out and 'still here' not in out,
          'release replay with a capture directory ends at the end of the route', out + err)
    check(out.count('exit_origin=') == 1 and 'exit_origin=input_route_capture_complete' in out,
          'the exit at the end of the route is named for the run report', out)
    check(sorted(p.name for p in folder.glob('frame-*.png')) ==
          [f'frame-{n:06d}.png' for n in (0, 3, 6, 7)],
          'a picture at every third boundary and at the end of the route',
          str(sorted(p.name for p in folder.iterdir())))
    for boundary in (0, 3, 6, 7):
        size, rows = png_rows(folder / f'frame-{boundary:06d}.png')
        held = words[boundary - 1] & 0xff if boundary else 0
        check(size == (4, 2) and rows == [[(boundary + 1, x * 16 + y, held) for x in range(4)]
                                           for y in range(2)],
              f'the picture at boundary {boundary} is the display at that boundary', str(rows))
    rows = [json.loads(line) for line in (folder / 'checkpoints.jsonl').read_text().splitlines()]
    check([row['frame'] for row in rows] == [0, 3, 6, 7] and
          all(row['sio_samples'] == row['frame'] and row['width'] == 4 and row['height'] == 2 and
              row['applied_words_sha256'] == row['supplied_words_sha256'] == words_sha(words[:row['frame']])
              for row in rows),
          'each checkpoint names the words delivered up to its boundary', str(rows))
    done = json.loads((folder / 'complete.json').read_text())
    end = json.loads((folder / 'input-end.json').read_text())
    check(done['frame'] == end['frame'] == 7 and done['input_frames'] == 7 and
          done['neutral_tail_ticks'] == 0 and done['applied_words_sha256'] == words_sha(words),
          'the completion record covers the whole route', str(done))

    # A delivery that differs from the route stops the run, as on the diagnostic product.
    for fault in ('wrong', 'analog'):
        folder, capture = shots(f'pictures-{fault}')
        code, out, err = pictures(seven, 20, capture, fault)
        check(code == 3 and 'SIO delivery differs from digital route' in err and
              not (folder / 'complete.json').exists(),
              f'a {fault} delivery to SIO stops the capture', out + err)
        check(out.count('exit_origin=') == 1 and 'exit_origin=input_route_capture_failed' in out,
              f'a capture stopped by a {fault} delivery is reported as failed', out)
    # A run that fails at its last boundary is not reported as a finished
    # capture: the last picture cannot be created, so no completion record.
    folder, capture = shots('pictures-last-boundary')
    capture['PSX_INPUT_ROUTE_CAPTURE_EVERY'] = '3'
    (folder / 'frame-000007.png').write_bytes(b'held')
    code, out, err = pictures(seven, 20, capture)
    check(code == 3 and (folder / 'frame-000006.png').is_file() and
          not (folder / 'complete.json').exists() and
          out.count('exit_origin=') == 1 and 'exit_origin=input_route_capture_failed' in out,
          'a capture that fails at its last boundary is reported as failed', out + err)

    # The other formats: PSXRTI3 without identity, and a DualShock route,
    # which declares its memory cards (none here).
    folder, capture = shots('pictures-v3')
    code, out, err = pictures(bare, 20, capture)
    check(code == 0 and sorted(p.name for p in folder.glob('frame-*.png')) ==
          ['frame-000000.png', 'frame-000003.png'], 'a PSXRTI3 route is captured', out + err)
    # A PSXRTI3 digital route has the same need as PSXRTI1: a pad that is not
    # in analog mode.
    folder, capture = shots('pictures-v3-analog')
    code, out, err = pictures(bare, 20, capture, 'analog')
    check(code == 3 and 'SIO delivery differs from digital route' in err and
          'exit_origin=input_route_capture_failed' in out,
          'a PSXRTI3 route delivered to an analog pad stops the capture', out + err)
    folder, capture = shots('pictures-dual')
    code, out, err = pictures(dual, 20, capture)
    done = json.loads((folder / 'complete.json').read_text()) if code == 0 else {}
    check(code == 0 and done.get('frame') == 2 and
          done.get('applied_controller_sha256') == done.get('expected_protocol_sha256') and
          json.loads((folder / 'initial-cards.json').read_text())['card1_present'] is False,
          'a DualShock route is captured with its protocol check', out + err)
    folder, capture = shots('pictures-dual-card')
    code, out, err = pictures(dual, 20, capture, PSX_TEST_CARD='1')
    check(code == 3 and 'initial card presence differs' in err and
          not list(folder.glob('frame-*.png')) and
          'exit_origin=input_route_capture_failed' in out,
          'a DualShock route with an undeclared card is not captured', out + err)

    # Only the directory and the interval are honoured. An evidence option of
    # the diagnostic product refuses the route by name; "0" is its off value.
    for name, value in (('PSX_INPUT_ROUTE_WATCH_U16', '0'), ('PSX_INPUT_ROUTE_CARD1_SHA256', '0' * 64),
                        ('PSX_INPUT_ROUTE_NEUTRAL_TAIL', '1'), ('PSX_INPUT_ROUTE_CPU_STATE', '1'),
                        ('PSX_INPUT_ROUTE_VIDEO_STATE', '1'), ('PSX_INPUT_ROUTE_TRACE', '1')):
        folder, capture = shots(f'pictures-{name}')
        code, out, err = pictures(seven, 20, capture, **{name: value})
        check(code == 30 and name in err and 'needs the diagnostic product' in err and
              'PSX_INPUT_ROUTE_CAPTURE_EVERY only' in err and 'exit_origin' not in out and
              not list(folder.iterdir()), f'{name} is refused on a release product', out + err)
        code, out, err = pictures(seven, 20, None, **{name: value})
        check(code == 8, f'{name} without a capture directory changes nothing', out + err)
    folder, capture = shots('pictures-off-values')
    code, out, err = pictures(seven, 20, capture, PSX_INPUT_ROUTE_NEUTRAL_TAIL='0',
                              PSX_INPUT_ROUTE_CPU_STATE='0', PSX_INPUT_ROUTE_VIDEO_STATE='0',
                              PSX_INPUT_ROUTE_TRACE='0')
    check(code == 0 and (folder / 'complete.json').is_file(), 'an option set to 0 is off', out + err)
    for label, capture in (('an empty capture directory', {'PSX_INPUT_ROUTE_CAPTURE_DIR': ''}),
                           ('a capture interval of 0', dict(shots('pictures-interval')[1],
                                                            PSX_INPUT_ROUTE_CAPTURE_EVERY='0'))):
        code, out, err = pictures(seven, 20, capture)
        check(code == 30 and 'not valid' in err, f'{label} refuses the route', out + err)
    # Evidence is never overwritten: a directory that already holds a capture stops the start.
    folder, capture = shots('pictures-collision')
    (folder / 'checkpoints.jsonl').write_bytes(b'preserve')
    code, out, err = pictures(seven, 20, capture)
    check(code == 3 and (folder / 'checkpoints.jsonl').read_bytes() == b'preserve' and
          'exit_origin=input_route_capture_failed' in out,
          'a capture directory that holds a capture is not overwritten', out + err)
    # The completion record of an earlier run refuses the route: at exit it is
    # what tells a finished capture from a failed one.
    folder, capture = shots('pictures-finished')
    (folder / 'complete.json').write_bytes(b'earlier')
    code, out, err = pictures(seven, 20, capture)
    check(code == 30 and 'already holds a finished capture' in err and
          (folder / 'complete.json').read_bytes() == b'earlier' and
          sorted(p.name for p in folder.iterdir()) == ['complete.json'],
          'a capture directory with a completion record refuses the route', out + err)

    # Release gate: nothing armed means every entry point is inert, and the
    # release product cannot record.
    code, out, err = run(release, 'inert', markers_exit=False)
    check(code == 0 and out.strip() == 'override=-1 boundary=-1 owns_ports=0 recording=0 dualshock=0',
          'release product is inert without PSX_INPUT_ROUTE_FILE', out + err)
    code, out, err = run(release, 'record', root / 'release.psxrti3', cue, bios, 10, markers_exit=False)
    check(code == 10 and 'diagnostic product' in err and not (root / 'release.psxrti3').exists(),
          'release product refuses to record', err)

print(f'input_route_session: {checks} checks passed')
