"""Spyro route adapter: title entries, profile, reference and route guards, without retail assets.

The real movie is checked too when the private library holds it (Pegasus), otherwise skipped.
"""
from pathlib import Path
import json
import tempfile

import biohazard
import dualshock_route
import nymashock28_route
import run_native
import spyro
import spyro2_admission
import spyro_admission
import tekken3
from replay_prefix import native_span


def rejects(call):
    try:
        call()
    except (ValueError, OSError):
        return
    raise AssertionError('invalid Spyro input admitted')


def pairs(profile):
    out = []
    for i, item in enumerate(profile):
        if item.startswith('--'):
            value = profile[i + 1] if i + 1 < len(profile) and not profile[i + 1].startswith('--') else None
            out.append((item, value))
    return sorted(out)


assert set(spyro.TITLES) - {'spyro2-5278M'} <= set(spyro_admission.TITLES)
entry, spec = spyro.title_spec('spyro3-4083M')
assert spec['host'] == '2.3' and spec['frames'] == 80352 and entry['bios_stem'] == 'SCPH5501'
assert entry['boot'] == 'SCUS_944.67' and entry['game_id'] == 'SCUS-94467'
assert entry['system_cnf'].startswith(b'BOOT = cdrom:\\' + entry['boot'].encode() + b';1\r\n')
assert entry['disc_stem'] + '.cue' in spec['media'] and entry['disc_stem'] + '.bin' in spec['media']
assert entry['reader'] == {'emu_versions': ('Version 2.3.0',)}
rejects(lambda: spyro.title_spec('crash-7798S'))

# Every Spyro profile delivers the logged stick bytes unscaled and answers as a DualShock.
for name, item in spyro.TITLES.items():
    profile = item['profile']
    assert profile[profile.index('--axis-model') + 1] == spyro.AXIS_MODEL == 'octoshock-byte', name
    assert spyro.AXIS_MODEL in run_native.AXIS_MODELS
    assert profile[profile.index('--pad-ack-model') + 1] != 'octoshock-2.2.2-digital', name
    assert profile[profile.index('--legacy-card-repair') + 1] == 'off'
    assert '--cpu-return-probe' in profile and '--ram-page-probe' in profile
    text = Path(run_native.__file__).read_text()
    for flag, _ in pairs(profile):
        assert ('"%s"' % flag) in text or ("'%s'" % flag) in text, flag
# The 2.3 set is Abe's Oddysee's (tekken3 plus the 2.3 CD-DA and MDEC models), with the DualShock
# ACK model in place of the digital one, plus the raw axis model.
expected = [x if x != 'octoshock-2.2.2-digital' else 'nymashock-1.29.0-dualshock' for x in tekken3.PROFILE]
expected += ['--cd-cdda-model', 'octoshock-2.3', '--mdec-source-model', 'octoshock-2.3', '--axis-model', 'octoshock-byte']
assert pairs(spyro.PROFILE_23) == pairs(expected) and len(spyro.PROFILE_23) == len(expected)
assert entry['profile'] is spyro.PROFILE_23

# 5523M: its own disc and boot program, the resaved 2.7 reader, and MODE only under the DTR-edge model.
e1, s1 = spyro.title_spec('spyro1-5523M')
assert s1['host'] == '2.7' and s1['frames'] == 128104 and e1['bios_stem'] + '.BIN' == s1['firmware'] == 'SCPH5501.BIN'
assert e1['reader'] == {'emu_versions': ('Version 2.7.0',), 'allow_resaved': True} and e1['disc_stem'] + '.bin' in s1['media']
assert e1['system_cnf'] == b'BOOT = cdrom:\\SCUS_942.28;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFFF0\r\n'
assert e1['profile'] == [*spyro.PROFILE_27, '--mode-button-model', 'octoshock-2.4.2'] and 'card1_sha256' not in e1

# 10,000 returns of the 80,352-input movie: 10,001 records, no tail.
assert native_span(spec['frames'], spec['frames'] + 3600, 10000) == (10001, 0)
assert native_span(spec['frames'], spec['frames'] + 3600, spec['frames'] + 3600) == (spec['frames'], 3601)
rejects(lambda: native_span(spec['frames'], spec['frames'] + 3600, spec['frames']))

with tempfile.TemporaryDirectory() as directory:
    p = Path(directory) / 'reference.json'
    good = dict(schema=spec['schema'], source_qualification='pass', movie_sha256=spec['movie_sha256'],
                original_inputs=spec['frames'])
    for value in ({}, dict(good, schema=spyro_admission.TITLES['spyro1-5523M']['schema']),
                  dict(good, source_qualification='unqualified'),
                  dict(good, movie_sha256=spyro_admission.TITLES['spyro3-5021M']['movie_sha256']),
                  dict(good, original_inputs=spec['frames'] - 1),
                  dict(good, observed_returns=spec['frames'] + 6001, neutral_tail=6001),
                  dict(good, observed_returns=spec['frames'] + 3600, neutral_tail=3599),
                  dict(good, observed_returns=spec['frames'] + 3600, neutral_tail=3600, bindings=[],
                       ram_pages='ram-pages.tsv', terminal_ram='ram-frame-083952.bin')):
        p.write_text(json.dumps(value))
        rejects(lambda: spyro.verify_reference(p, 'spyro3-4083M'))
    s = Path(directory) / 'setup.json'
    for value in ({}, dict(schema='crash-tas-candidate-v1', title='spyro3-4083M'),
                  dict(schema=spyro.SCHEMA), dict(schema=spyro.SCHEMA, title='crash-7798S')):
        s.write_text(json.dumps(value))
        rejects(lambda: spyro.read_setup(s))
    s.write_text(json.dumps(dict(schema=spyro.SCHEMA, title='spyro3-4083M')))
    assert spyro.read_setup(s)['title'] == 'spyro3-4083M'
    # A wrong movie is refused before it is read.
    movie = Path(directory) / 'movie.bk2'
    movie.write_bytes(b'not the movie')
    rejects(lambda: spyro.read_route(movie, 'spyro3-4083M'))

# A movie that presses MODE, or is short, is refused even when its bytes are the pinned movie's.
saved = spyro.require_hash, dualshock_route.read_movie_octoshock_dualshock
try:
    spyro.require_hash = lambda path, expected: None
    s1full = [(0xFFFF, 128, 128, 128, 128, 0)] * s1['frames']
    dualshock_route.read_movie_octoshock_dualshock = lambda movie, **kw: (s1full, {'mode_press_frames': 4})
    assert spyro.read_route(Path('movie.bk2'), 'spyro1-5523M') == s1full
    full = [(0xFFFF, 128, 128, 128, 128, 0)] * spec['frames']
    for rows, facts in ((full, {'mode_press_frames': 1}), (full[:-1], {'mode_press_frames': 0})):
        dualshock_route.read_movie_octoshock_dualshock = lambda movie, rows=rows, facts=facts, **kw: (rows, facts)
        rejects(lambda: spyro.read_route(Path('movie.bk2'), 'spyro3-4083M'))
    dualshock_route.read_movie_octoshock_dualshock = lambda movie, **kw: (full, {'mode_press_frames': 0, 'kw': kw})
    assert spyro.read_route(Path('movie.bk2'), 'spyro3-4083M') == full
finally:
    spyro.require_hash, dualshock_route.read_movie_octoshock_dualshock = saved

# Spyro 2 5278M: a BizHawk 2.8 Nymashock movie with raw u16 sticks, admitted by spyro2_admission.py.
entry2, spec2 = spyro.title_spec('spyro2-5278M')
assert spec2['tool'] is spyro2_admission and spec2['roles'] == ('stock-control', 'pages-observer')
assert spec2['frames'] == spyro2_admission.FRAMES == 86655 and spec2['max_tail'] == 12000
assert spec2['movie_sha256'] == spyro2_admission.MOVIE_SHA and spec2['schema'] == 'spyro2-independent-source-v1'
assert spec2['firmware_sha256'] == '11052b6499e466bbf0a709b1f9cb6834a9418e66680387912451e971cf8a1fef'
assert entry2['bios_stem'] == 'SCPH5501' and entry2['boot'] == 'SCUS_944.25' and entry2['game_id'] == 'SCUS-94425'
assert entry2['system_cnf'].startswith(b'BOOT = cdrom:\\' + entry2['boot'].encode() + b';1\r\n')
assert set(spec2['media']) == {entry2['disc_stem'] + '.cue', entry2['disc_stem'] + '.bin'}
assert entry2['route_reader'] == 'nymashock28'
assert entry2['reader'] == {'mednafen_values': {'psx.input.port1.memcard': '0'}}
# The spyro3 title keeps spyro_admission's roles and tail bound.
assert spec['tool'] is spyro_admission and spec['roles'] == ('stock', 'observer') and spec['max_tail'] == 6000
# Bio Hazard's Nymashock set, minus its memory card model (this movie declares port 1 empty), plus the
# raw axis model, with the Octoshock Init draw order (PS1G-125); everything else is the same
# selector for selector.
card = biohazard.PROFILE.index('--card-model')
expected2 = biohazard.PROFILE[:card] + biohazard.PROFILE[card + 2:] + ['--axis-model', 'octoshock-byte']
drive = expected2.index('--cd-drive-model')
assert expected2[drive + 1] == 'nymashock-1.29.0'
expected2[drive + 1] = 'octoshock-2.7'
assert pairs(entry2['profile']) == pairs(expected2)
assert entry2['profile'][entry2['profile'].index('--cd-drive-model') + 1] == 'octoshock-2.7'
assert '--card-model' not in entry2['profile'] and entry2['profile'] is spyro.PROFILE_NYMA28
assert native_span(spec2['frames'], spec2['frames'] + 3600, 10000) == (10001, 0)
assert native_span(spec2['frames'], spec2['frames'] + 3600, spec2['frames'] + 3600) == (spec2['frames'], 3601)

with tempfile.TemporaryDirectory() as directory:
    p2 = Path(directory) / 'reference.json'
    good2 = dict(schema=spec2['schema'], source_qualification='pass', movie_sha256=spec2['movie_sha256'],
                 original_inputs=spec2['frames'])
    for value in ({}, dict(good2, schema=spec['schema']), dict(good2, source_qualification='unqualified'),
                  dict(good2, movie_sha256=spec['movie_sha256']), dict(good2, original_inputs=spec['frames']),
                  dict(good2, observed_returns=spec2['frames'] + 12001, neutral_tail=12001),
                  dict(good2, observed_returns=spec2['frames'] + 3600, neutral_tail=3599),
                  dict(good2, observed_returns=spec2['frames'] + 3600, neutral_tail=3600, bindings=[],
                       ram_pages='ram-pages.tsv', terminal_ram='ram-frame-090255.bin')):
        p2.write_text(json.dumps(value))
        rejects(lambda: spyro.verify_reference(p2, 'spyro2-5278M'))
    # A spyro2 reference is not a spyro3 one.
    p2.write_text(json.dumps(good2))
    rejects(lambda: spyro.verify_reference(p2, 'spyro3-4083M'))
    s2 = Path(directory) / 'setup.json'
    s2.write_text(json.dumps(dict(schema=spyro.SCHEMA, title='spyro2-5278M')))
    assert spyro.read_setup(s2)['title'] == 'spyro2-5278M'
    movie2 = Path(directory) / 'movie.bk2'
    movie2.write_bytes(b'not the movie')
    rejects(lambda: spyro.read_route(movie2, 'spyro2-5278M'))

# The 2.8 reader is called with the movie's bytes and the declared card setting; MODE or a short
# movie is refused even when the bytes are the pinned movie's.
saved2 = spyro.require_hash, nymashock28_route.read_movie_nymashock28
try:
    spyro.require_hash = lambda path, expected: None
    with tempfile.TemporaryDirectory() as directory:
        fake = Path(directory) / 'movie.bk2'
        fake.write_bytes(b'bytes')
        full2 = [(0xFFFF, 128, 128, 128, 128, 0)] * spec2['frames']
        for rows, facts in ((full2, {'mode_press_frames': 1}), (full2[:-1], {'mode_press_frames': 0})):
            nymashock28_route.read_movie_nymashock28 = lambda movie, rows=rows, facts=facts, **kw: (rows, facts)
            rejects(lambda: spyro.read_route(fake, 'spyro2-5278M'))
        seen = {}

        def reader(movie, **kw):
            seen.update(movie=movie, kw=kw)
            return full2, {'mode_press_frames': 0}
        nymashock28_route.read_movie_nymashock28 = reader
        assert spyro.read_route(fake, 'spyro2-5278M') == full2
        assert seen == {'movie': fake, 'kw': {'mednafen_values': {'psx.input.port1.memcard': '0'}}}
finally:
    spyro.require_hash, nymashock28_route.read_movie_nymashock28 = saved2

REAL2 = [Path(r'D:\Projects\Recompilations\PS1\Library\Movies\tas') / 'lapogne36-spyro2-moneybags.bk2',
         Path(__file__).resolve().parent / 'movies' / 'lapogne36-spyro2-moneybags.bk2',
         Path(r'C:\Projects\Recompilations\PS1\Work\PS1G-125-spyro2-5278m\inputs\lapogne36-spyro2-moneybags.bk2')]
real2 = next((x for x in REAL2 if x.is_file()), None)
if real2:
    rows2 = spyro.read_route(real2, 'spyro2-5278M')
    with tempfile.TemporaryDirectory() as directory:
        route2 = Path(directory) / 'input.psxrti2'
        receipt2 = dualshock_route.write_route(rows2, route2)
        assert receipt2['canonical_controller_sha256'] == entry2['controller_sha256']
        identity2 = run_native.route_identity(route2, axis_model=spyro.AXIS_MODEL)
        assert identity2['frames'] == spec2['frames'] and identity2['expected_protocol_sha256'] == entry2['controller_sha256']
    print('spyro2-5278M real movie: %d frames, controller %s' % (len(rows2), entry2['controller_sha256'][:12]))
else:
    print('spyro2-5278M real movie absent; real-movie checks skipped')

LIBRARY = Path(r'D:\Projects\Recompilations\PS1\Library\Movies\tas')
for title in sorted(spyro.TITLES):
    entry, spec = spyro.title_spec(title)
    real = LIBRARY / spec['movie']
    if real.is_file():
        rows = spyro.read_route(real, title)
        with tempfile.TemporaryDirectory() as directory:
            route = Path(directory) / 'input.psxrti2'
            receipt = dualshock_route.write_route(rows, route)
            assert receipt['canonical_controller_sha256'] == entry['controller_sha256']
            mode_flag = '--mode-button-model'
            mode_model = entry['profile'][entry['profile'].index(mode_flag) + 1] if mode_flag in entry['profile'] else 'none'
            if any(row[5] for row in rows):
                rejects(lambda: run_native.route_identity(route, axis_model=spyro.AXIS_MODEL))
            identity = run_native.route_identity(route, axis_model=spyro.AXIS_MODEL, mode_model=mode_model)
            assert identity['frames'] == spec['frames'] and identity['expected_protocol_sha256'] == entry['controller_sha256']
        print('%s real movie: %d frames, controller %s' % (title, len(rows), entry['controller_sha256'][:12]))
    else:
        print('%s real movie absent; real-movie checks skipped' % title)
print('Spyro adapter: title entries, raw-axis DualShock profile, reference/setup/route guards and 10k span pass')
