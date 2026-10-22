"""PSXRTI3 two-digital-pad routes in run_native.py: route identity and the disc roster.

The runtime binds an event route to the discs game.toml [game] discs names and boots the disc
--disc names. run_native.disc_roster admits a run only when --roster-disc, game.toml and the
route's disc set name the same images in the same order, with --disc first. Authored discs only.
"""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_native import checkpoint_asset_digest, disc_roster, playback_identity_matches, route_identity

checks = 0


def check(ok, what):
    global checks
    if not ok:
        raise SystemExit(f'FAIL: {what}')
    checks += 1


def refused(call, what):
    try:
        call()
    except ValueError:
        return check(True, what)
    check(False, what)


def entry(tag, payload):
    return struct.pack('<II', tag, len(payload)) + payload + bytes(-len(payload) % 4)


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    cues = []
    for n in (1, 2):
        (root / f'Game (Disc {n}).bin').write_bytes(bytes([n]) * 4704)
        cue = root / f'Game (Disc {n}).cue'
        cue.write_text(f'FILE "Game (Disc {n}).bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
        cues.append(cue)
    digests = [checkpoint_asset_digest(c) for c in cues]
    pads = [(0xfffe, 0xffff), (0xffff, 0xfffd), (0xffff, 0xffff), (0xfff7, 0xffff)]
    events = [(1, 2, 0), (2, 4, 2), (2, 3, 2)]
    ext = (entry(0x201, bytes((2, 0, 0, 0, 1, 1))) +
           entry(0x202, struct.pack('<I', len(events)) + b''.join(struct.pack('<IBBxx', *e) for e in events)) +
           entry(0x203, struct.pack('<I', 2) + b''.join(bytes((1, 0, 0, 0)) + bytes.fromhex(d) for d in digests)))
    route = root / 'two.psxrti3'
    route.write_bytes(struct.pack('<8sIIIII', b'PSXRTI3\0', 3, 12, len(pads), 0, len(ext)) + ext +
                      b''.join(struct.pack('<IHHHH', i + 1, p1, 0, p2, 0) for i, (p1, p2) in enumerate(pads)))
    identity = route_identity(route)
    check(identity['format'] == 'PSXRTI3' and identity['frames'] == 4 and identity['steps'] == 4,
          'two-pad identity reads the records')
    check(identity['words_sha256'] == hashlib.sha256(b''.join(struct.pack('<H', a) for a, _ in pads)).hexdigest() and
          identity['port2_words_sha256'] == hashlib.sha256(b''.join(struct.pack('<H', b) for _, b in pads)).hexdigest(),
          'P1 and P2 words are hashed apart')
    check([e['event'] for e in identity['console_events']] == ['TRAY_OPEN', 'DISC_SELECT', 'TRAY_CLOSE'],
          'console events are named in file order')
    resumed = route_identity(route, 2)
    check(resumed['port2_words_sha256'] == hashlib.sha256(b''.join(struct.pack('<H', b) for _, b in pads[2:])).hexdigest(),
          'a start record drops earlier P2 words too')

    game = root / 'game.toml'
    game.write_text('[game]\ndiscs = ["Game (Disc 1).cue", "Game (Disc 2).cue"]\n', encoding='utf-8')
    roster = disc_roster(identity, cues[0].resolve(), cues, game)
    check([r['path'] for r in roster] == [str(c.resolve()) for c in cues] and
          [r['digest'] for r in roster] == digests, 'the roster binds both discs in route order')
    refused(lambda: disc_roster(identity, cues[0].resolve(), cues[:1], game), 'a missing roster disc is refused')
    refused(lambda: disc_roster(identity, cues[1].resolve(), cues[::-1], game), 'a reordered roster is refused')
    refused(lambda: disc_roster(identity, cues[1].resolve(), cues, game), '--disc must be the first roster disc')
    swapped = root / 'swapped.toml'
    swapped.write_text('[game]\ndiscs = ["Game (Disc 2).cue", "Game (Disc 1).cue"]\n', encoding='utf-8')
    refused(lambda: disc_roster(identity, cues[0].resolve(), cues, swapped), 'game.toml in another order is refused')
    single = root / 'single.toml'
    single.write_text('[game]\nid = "X"\n', encoding='utf-8')
    refused(lambda: disc_roster(identity, cues[0].resolve(), cues, single), 'game.toml without discs is refused')
    check(disc_roster({'format': 'PSXRTI1'}, cues[0].resolve(), [], game) is None, 'a route without a disc set needs no roster')
    refused(lambda: disc_roster({'format': 'PSXRTI1'}, cues[0].resolve(), cues, game), 'a roster without a disc set is refused')
    (root / 'Game (Disc 2).bin').write_bytes(b'\x09' * 4704)
    refused(lambda: disc_roster(identity, cues[0].resolve(), cues, game), 'a changed disc image is refused')

    complete = {'frame': 5, 'input_frames': 4, 'neutral_tail_ticks': 1, 'port_layout': 'two-digital',
                'applied_words_sha256': identity['words_sha256'],
                'applied_port2_words_sha256': identity['port2_words_sha256'],
                'supplied_port2_words_sha256': identity['port2_words_sha256']}
    check(playback_identity_matches(complete, identity, 1), 'a completion with both ports matches')
    check(not playback_identity_matches(dict(complete, applied_port2_words_sha256='0' * 64), identity, 1),
          'a different applied P2 hash fails')
    check(not playback_identity_matches({k: v for k, v in complete.items() if k != 'port_layout'}, identity, 1),
          'a single-port completion fails')

print(f'run_native two-pad roster: {checks} checks passed')
