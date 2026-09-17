"""Record -> replay round trip and release gate for input_route_session.c.

usage: test_input_route_session.py DIAGNOSTIC_EXE RELEASE_EXE

DIAGNOSTIC_EXE and RELEASE_EXE are test_input_route_session.c built without
and with PSX_NO_DEBUG_TOOLS. Authored discs and routes only.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

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


def run(exe, *args, markers_exit=True):
    env = dict(os.environ)
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
    broken = root / 'broken.psxrti'
    broken.write_bytes(legacy.read_bytes()[:-1])
    code, out, err = run(release, 'words', broken, 1, markers_exit=False)
    check(code == 30 and 'input route rejected' in err, 'malformed route refused before replay', err)

    # Release gate: nothing armed means every entry point is inert, and the
    # release product cannot record.
    code, out, err = run(release, 'inert', markers_exit=False)
    check(code == 0 and out.strip() == 'override=-1 boundary=-1 owns_ports=0 recording=0 dualshock=0',
          'release product is inert without PSX_INPUT_ROUTE_FILE', out + err)
    code, out, err = run(release, 'record', root / 'release.psxrti3', cue, bios, 10, markers_exit=False)
    check(code == 10 and 'diagnostic product' in err and not (root / 'release.psxrti3').exists(),
          'release product refuses to record', err)

print(f'input_route_session: {checks} checks passed')
