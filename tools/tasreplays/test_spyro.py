"""Spyro route adapter: title entries, profile, reference and route guards, without retail assets.

The real movie is checked too when the private library holds it (Pegasus), otherwise skipped.
"""
from pathlib import Path
import json
import tempfile

import dualshock_route
import run_native
import spyro
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


assert set(spyro.TITLES) <= set(spyro_admission.TITLES)
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
    full = [(0xFFFF, 128, 128, 128, 128, 0)] * spec['frames']
    for rows, facts in ((full, {'mode_press_frames': 1}), (full[:-1], {'mode_press_frames': 0})):
        dualshock_route.read_movie_octoshock_dualshock = lambda movie, rows=rows, facts=facts, **kw: (rows, facts)
        rejects(lambda: spyro.read_route(Path('movie.bk2'), 'spyro3-4083M'))
    dualshock_route.read_movie_octoshock_dualshock = lambda movie, **kw: (full, {'mode_press_frames': 0, 'kw': kw})
    assert spyro.read_route(Path('movie.bk2'), 'spyro3-4083M') == full
finally:
    spyro.require_hash, dualshock_route.read_movie_octoshock_dualshock = saved

LIBRARY = Path(r'D:\Projects\Recompilations\PS1\Library\Movies\tas')
real = LIBRARY / spec['movie']
if real.is_file():
    rows = spyro.read_route(real, 'spyro3-4083M')
    with tempfile.TemporaryDirectory() as directory:
        route = Path(directory) / 'input.psxrti2'
        receipt = dualshock_route.write_route(rows, route)
        assert receipt['canonical_controller_sha256'] == entry['controller_sha256']
        identity = run_native.route_identity(route, axis_model=spyro.AXIS_MODEL)
        assert identity['frames'] == spec['frames'] and identity['expected_protocol_sha256'] == entry['controller_sha256']
    print('spyro3-4083M real movie: %d frames, controller %s' % (len(rows), entry['controller_sha256'][:12]))
else:
    print('spyro3-4083M real movie absent; real-movie checks skipped')
print('Spyro adapter: title entries, raw-axis DualShock profile, reference/setup/route guards and 10k span pass')
