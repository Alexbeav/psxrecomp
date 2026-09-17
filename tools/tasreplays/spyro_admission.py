"""Admit an independently replayed Octoshock Spyro source pair, never native output.

Copy of octoshock2x_admission.py for the three Spyro titles: Spyro the Dragon 5523M and Spyro:
Year of the Dragon 5021M on BizHawk 2.7, and Year of the Dragon 4083M on BizHawk 2.3. A copy rather
than three more entries there, because that module's own bytes are hashed into the admitted Crash,
Abe's Oddysee and Exoddus references and every one of those adapters refuses a reference whose
admission_tool_sha256 differs.

Four differences from it, each forced by these movies:
- 2.3 runs use `source_control_octo23ds.py` and `source_control_octo23ds.lua` (derive_octo23ds.py).
  These movies carry a DualShock, and the 2.3 lua refused every numeric control except Disc Select
  in the neutral tail; the variant holds the four stick axes at their neutral 128 instead.
- The 2.7 launcher hash is the revision that accepts the 2.9.1-resaved container of a 2.7.0
  recording (Spyro the Dragon 5523M is published as a 2.7 movie but was re-saved by 2.9.1).
- SCPH-1001 is admissible, because Spyro 3's 100-eggs movie declares it (SHA-1 10155d8d).
- The 2.10 host is dropped; no Spyro title uses it.

This is the Pepsiman admission (admit_source_control / verify_control_identity / reference)
restated for those launchers, with every host, helper, core, movie, media and firmware identity
pinned per title. Same rules: a source pass is admitted only with a witnessed semantic review
naming one of its own captured frames, a clean exit with the original movie unchanged, complete
return coverage, and the stock/observer pair byte-identical in RAM digests, lag, screenshots and
effective identity. The observer role is admissible only through that comparison, because it runs
a rebuilt core.

The three hosts differ in ways the HOSTS fields below record, all verified against real runs:
2.3 loads its observer helper (Observation230) in the observer role only, so its stock pass writes
no loaded-core.json and the helper is absent from the stock binding closure; 2.7 and 2.10 lost
BizHawk.Client.Common.Global, so ObservationOcto2x is loaded in both roles and also writes
effective-run-policy.json and helper-build.json.
"""
from pathlib import Path
import argparse, hashlib, json, re, zipfile
from observation_evidence import captured_frames, compare_stock_observations, terminal_consistency

HOSTS = {
    '2.3': {'schema': 'octoshock23-stock-control-v1',
            'launcher': 'source_control_octo23ds.py', 'lua': 'source_control_octo23ds.lua',
            'lua_sha256': '8222ae0b85df781cb773e7326c28f0a9d970fba33ef16728c90b8d50c05a0e18',
            'launcher_sha256': {'c47d1ad828fa5b97d0c3157d76e998882af261551c4164677a1e75dc5b0ebc0b': 'DualShock 2.3 launcher (derive_octo23ds.py)'},
            'helper_dll': 'Observation230.dll', 'helper_src': 'Observation230.cs',
            'helper_dll_sha256': 'ecdaf690d9bd4893759faa129f088a7d6afb23d4cc6a3d3b5c1b9b872c1610b9',
            'helper_src_sha256': '4d9e880cb994d7c3bb09e351376daa83fa5df9bce2d138e21d478f4a2965c3d3',
            'stock_loads_helper': False, 'has_run_policy': False, 'has_host_archive': False,
            'has_helper_build': False,
            'emuhawk_sha256': '0859ec057a98f55d1e74cdd265c5a75281c7d58b15b4576b12ede3ab7deb352e',
            'stock_core_sha256': '749d6dd58430d010e46ae97c628e113adad5f420c05c2ada0ad35e58191781c0',
            'stock_commit': 'a15b31a46bdac27d843d3ebbc5a860012d8452fb',
            'observer_core_sha256': 'fce723aa1e2b73ede285863d4b1841445be47e5dd2ccb054b2e170830a7b5167',
            'observer_head': 'b3ec859cc082d13b9d229dba2036fefeee96289d'},
    '2.7': {'schema': 'octoshock2x-stock-control-v1',
            'launcher': 'source_control_octo2x.py', 'lua': 'source_control_octo2x.lua',
            'lua_sha256': '5bf2ea992918bba219b31932d2cf47212c113c497e72a23b0f8db6ef3364eedf',
            # Two launcher revisions are admissible: the storage budget was raised from 1 GiB to
            # 6 GiB between the Crash control and observer passes (the 1 GiB cap cut the first
            # observer attempt at frame 120,733). Nothing about observation changed.
            'launcher_sha256': {'c8f05b08689a315aebf6e4e8f38d0a43e61d69c7a00ec3e34b7d2f50ff472cc3':
                                'accepts the 2.9.1-resaved container of a 2.7.0 recording'},
            'helper_dll': 'ObservationOcto2x.dll', 'helper_src': 'ObservationOcto2x.cs',
            'helper_dll_sha256': '0a88a73726f22ae9a9f481261d0341ca2f47f90c9efe60384696e109d8fd249c',
            'helper_src_sha256': '3da48f1741f851261d0609d3873896bedad9a0390a7b6c0af2e08e4c792335d1',
            'stock_loads_helper': True, 'has_run_policy': True, 'has_host_archive': True,
            'has_helper_build': True,
            'archive': 'BizHawk-2.7-win-x64.zip',
            'archive_sha256': 'e7e69bbe10c52ef050ed0f4e09369c15c06fad8925073c1346b37dfe680e42b2',
            'emuhawk_sha256': '9cc462d0d0a3c42122ca8ee13f5665dbdecd41b8488ec1d7c3ebfedc288ab9ae',
            'stock_core_sha256': '2255db4c215f680257bc392e77c45f6cd160807a7ede37a6b8b43b38ac1ab3d0',
            'stock_commit': 'dbaf2595625f79093eeec37d2d4a7a9a4d37f370',
            'observer_core_sha256': '1d51965615a4d6bb223889fc1c78cda9643ddc917d0be8dc0d435d2b772c8c24',
            'observer_head': '2fe0a892e21d7b192a4b2bff73b77535ede7ced6'},
}
FIRMWARE = {
    'SCPH5500.BIN': ('PSX+J', '9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb', 'b05def971d8ec59f346f2d9ac21fb742e3eb6917'),
    'SCPH5501.BIN': ('PSX+U', '11052b6499e466bbf0a709b1f9cb6834a9418e66680387912451e971cf8a1fef', '0555c6fae8906f3f09baf5988f00e55f88e9f30b'),
    # Spyro: Year of the Dragon 5021M declares SCPH-1001 in its own header, not SCPH-5501.
    'SCPH1001.BIN': ('PSX+U', '71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3', '10155d8d6e6e832d6ea66db9bc098321fb5e8ebf'),
}
TITLES = {
    'spyro1-5523M': {
        'title': 'Spyro the Dragon (USA) any% 5523M', 'host': '2.7', 'frames': 128104,
        'movie': 'toastedkat_wafflewizard1-spyrothedragon.bk2',
        'movie_sha256': 'd3682741efc4ed11bf2143d68d172295d3055c531e60f9f020e7c63465e9b29e',
        'firmware': 'SCPH5501.BIN',
        'media': {'Spyro the Dragon (USA).bin': 'fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37',
                  'Spyro the Dragon (USA).cue': '05142502c75d3a0b6374003a8fe41893d6faf0ad55863ff0b62a265a1b152766'},
        'sync': {'EnableLEC': False, 'FIOConfig': {'Multitaps': [False, False], 'Memcards': [False, False], 'Devices8': [2, 0, 0, 0, 0, 0, 0, 0]}},
        'schema': 'spyro1-independent-source-v1'},
    'spyro3-4083M': {
        'title': 'Spyro Year of the Dragon (USA) v1.0 any% 4083M', 'host': '2.3', 'frames': 80352,
        'movie': 'nitrofski_lapogne36-spyroyotd.bk2',
        'movie_sha256': 'ba877f8f6f3acf49fd41f16dce011940ee51fd6e93fdea8f9bee89eabb566e63',
        'firmware': 'SCPH5501.BIN',
        'media': {'Spyro - Year of the Dragon (USA).bin': 'a313c9b1306a08e950edddc152280a443796e68052c8e0454b5d76d69354d157',
                  'Spyro - Year of the Dragon (USA).cue': 'db72b1dc2a19fd821b52f7dd5854f66fad49d7b26c72f9c9db7cbd7082126e9a'},
        'sync': {'EnableLEC': False, 'FIOConfig': {'Multitaps': [False, False], 'Memcards': [False, False], 'Devices8': [2, 0, 0, 0, 0, 0, 0, 0]}},
        'schema': 'spyro3-independent-source-v1'},
    'spyro3-5021M': {
        # The only lane title with a memory card in port 1, and the only one on SCPH-1001.
        'title': 'Spyro Year of the Dragon (USA) v1.0 100 eggs 5021M', 'host': '2.7', 'frames': 163115,
        'movie': 'wafflewizard1_jeremythompson-spyroyotd-100eggs.bk2',
        'movie_sha256': '881f24502be44aaec2532b7d4ad0c15977104bbe67096795e7093cc78054c365',
        'firmware': 'SCPH1001.BIN',
        'media': {'Spyro - Year of the Dragon (USA).bin': 'a313c9b1306a08e950edddc152280a443796e68052c8e0454b5d76d69354d157',
                  'Spyro - Year of the Dragon (USA).cue': 'db72b1dc2a19fd821b52f7dd5854f66fad49d7b26c72f9c9db7cbd7082126e9a'},
        'sync': {'EnableLEC': False, 'FIOConfig': {'Multitaps': [False, False], 'Memcards': [True, False], 'Devices8': [2, 0, 0, 0, 0, 0, 0, 0]}},
        'schema': 'spyro3-100eggs-independent-source-v1'},
}


def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def declared_firmware(movie):
    """The BIOS SHA-1 the movie's own header names, and the region key it names it under."""
    with zipfile.ZipFile(movie) as archive:
        header = archive.read('Header.txt').decode('utf-8', 'replace')
    named = [l.split(' ', 1) for l in header.splitlines() if l.startswith('PSX_Firmware_')]
    if len(named) != 1:
        raise ValueError('movie declares %d firmwares, expected exactly one' % len(named))
    return named[0][0], named[0][1].strip().lower()


def binding_names(bindings):
    """Basename -> binding. A basename may repeat only when every copy has the same bytes: the
    2.3 runs bind the same BIOS image twice, once through the media closure and once as firmware."""
    names = {}
    for item in bindings:
        name = Path(item['path']).name
        if name in names and names[name]['sha256'] != item['sha256']:
            raise ValueError('two different artifacts share the basename ' + name)
        names.setdefault(name, item)
    return names


def verify_identity(root, manifest, title, role, tail):
    spec = TITLES[title]
    host = HOSTS[spec['host']]
    key, bios_sha, bios_sha1 = FIRMWARE[spec['firmware']]
    frames = spec['frames']
    if (manifest.get('schema'), manifest.get('title'), manifest.get('role'), manifest.get('source_tag'),
        manifest.get('original_inputs'), manifest.get('cutoff'), manifest.get('neutral_tail'),
        manifest.get('firmware_key'), manifest.get('firmware_sha256')) != (
            host['schema'], spec['title'], role, spec['host'], frames, frames, tail, key, bios_sha):
        raise ValueError('source manifest role/host/firmware/input boundary differs')
    expected_commit = host['observer_head'] if role == 'observer' else host['stock_commit']
    if manifest.get('source_commit') != expected_commit:
        raise ValueError('source commit differs from the pinned %s revision' % role)
    if host['has_host_archive'] and manifest.get('host_archive', {}).get('sha256') != host['archive_sha256']:
        raise ValueError('source host archive differs')
    bindings = manifest['bindings']
    names = binding_names(bindings)
    for item in bindings:
        if digest(item['path']) != item['sha256']:
            raise ValueError('changed source artifact: ' + Path(item['path']).name)
    # EmuHawk rewrites the applied host config on exit; the launcher froze the applied bytes.
    applied = manifest.get('applied_host_config')
    if applied and applied.get('sha256') != names['launch-config.json']['sha256']:
        raise ValueError('frozen launch config differs from the applied host config')
    helper_here = role == 'observer' or host['stock_loads_helper']
    required = {*spec['media'], 'EmuHawk.exe', 'octoshock.dll', spec['movie'], 'original.bk2',
                spec['firmware'], 'launch-config.json', 'start.lua', host['lua'], host['launcher'],
                'host-closure.json'}
    if helper_here:
        required |= {host['helper_dll'], host['helper_src']}
    if host['has_helper_build']:
        required.add('helper-build.json')
    if role == 'observer':
        # --reference binds the stock control's ram-frames.tsv; qualify() checks it is that run's.
        required |= {'observer-build.json', 'ram-frames.tsv'}
    if set(names) != required:
        raise ValueError('source binding closure differs: %s' % sorted(set(names) ^ required))
    for name, expected in spec['media'].items():
        if names[name]['sha256'] != expected:
            raise ValueError('source media differs: ' + name)
    if names[spec['movie']]['sha256'] != spec['movie_sha256'] or names['original.bk2']['sha256'] != spec['movie_sha256']:
        raise ValueError('source movie differs')
    if names[spec['firmware']]['sha256'] != bios_sha:
        raise ValueError('source firmware differs')
    if names['EmuHawk.exe']['sha256'] != host['emuhawk_sha256']:
        raise ValueError('source host executable differs')
    if host['lua_sha256'] and names[host['lua']]['sha256'] != host['lua_sha256']:
        raise ValueError('source observer lua differs')
    if host['launcher_sha256'] and names[host['launcher']]['sha256'] not in host['launcher_sha256']:
        raise ValueError('source launcher differs')
    if helper_here and (names[host['helper_dll']]['sha256'] != host['helper_dll_sha256']
                        or names[host['helper_src']]['sha256'] != host['helper_src_sha256']):
        raise ValueError('observer helper differs')
    for name in ('original.bk2', 'launch-config.json', 'start.lua', host['lua'], host['launcher'],
                 'host-closure.json', 'helper-build.json', 'observer-build.json'):
        if name in names and Path(names[name]['path']).resolve() != root / name:
            raise ValueError('run-local source artifact escapes: ' + name)
    if host['has_helper_build']:
        helper = read(root / 'helper-build.json')
        if (helper.get('exit_code'), helper.get('host_tag'), helper.get('dll_sha256'), helper.get('source_sha256')) != (
                0, spec['host'], host['helper_dll_sha256'], host['helper_src_sha256']):
            raise ValueError('observer helper build provenance differs')
    for entry in read(root / 'host-closure.json'):
        if digest(entry['path']) != entry['sha256']:
            raise ValueError('changed stock host closure: ' + entry['path'])
    core = names['octoshock.dll']
    if helper_here:
        loaded = read(root / 'loaded-core.json')
        if loaded.get('sha256') != core['sha256'] or Path(loaded['path']).resolve() != Path(core['path']).resolve():
            raise ValueError('loaded core differs from its bound binary')
    elif (root / 'loaded-core.json').exists():
        raise ValueError('stock control on this host must not load the observer helper')
    if role == 'observer':
        if core['sha256'] != host['observer_core_sha256']:
            raise ValueError('observer core is not the pinned rebuilt getter build')
        build = read(root / 'observer-build.json')
        if (build.get('exit_code'), build.get('source_head'), build.get('upstream_commit'), build.get('core_sha256')) != (
                0, host['observer_head'], host['stock_commit'], core['sha256']):
            raise ValueError('observed core build provenance differs')
    elif core['sha256'] != host['stock_core_sha256']:
        raise ValueError('stock control must use the pinned unmodified release core')
    if read(root / 'loaded-bios.json') != {'sha256': bios_sha, 'start': 'power_on', 'movie_length': frames}:
        raise ValueError('loaded source BIOS/start identity differs')
    region, declared = declared_firmware(root / 'original.bk2')
    if region != 'PSX_Firmware_' + key.rsplit('+', 1)[1] or declared != bios_sha1:
        raise ValueError('movie declares firmware %s %s, pinned %s' % (region, declared, bios_sha1))
    if hashlib.sha1(Path(names[spec['firmware']]['path']).read_bytes()).hexdigest() != declared:
        raise ValueError('loaded BIOS is not the firmware the movie declares')
    if read(root / 'effective-sync.json') != spec['sync']:
        raise ValueError('source effective controller/card configuration differs')
    if read(root / 'effective-movie-policy.json') != {'read_only': True, 'movie_end_action': 3}:
        raise ValueError('source movie policy differs')
    if host['has_run_policy']:
        policy = read(root / 'effective-run-policy.json')
        if policy.get('read_only') is not True or policy.get('movie_end_action') != 'Finish':
            raise ValueError('source input/end policy differs')


def return_rows(path, endpoint):
    with Path(path).open() as rows:
        if rows.readline().strip() != 'frame\tlag_count\tpc\tram_sha256':
            raise ValueError('unrecognized stock RAM capture')
        count = 0
        last = None
        for expected, row in enumerate(rows):
            columns = row.rstrip('\r\n').split('\t')
            # The 2.7/2.10 lua prints the PC sign-extended to 16 hex digits; 2.3 printed 8.
            if (len(columns) != 4 or int(columns[0]) != expected
                    or not re.fullmatch('[0-9A-Fa-f]{8}|[0-9A-Fa-f]{16}', columns[2])
                    or not re.fullmatch('[0-9A-Fa-f]{64}', columns[3])):
                raise ValueError('incomplete or discontinuous stock RAM capture')
            count += 1
            last = columns
        if count != endpoint + 1:
            raise ValueError('stock control misses declared input/neutral boundaries')
    return last


def admit(root, title, role):
    root = Path(root).resolve(strict=True)
    spec = TITLES[title]
    host = HOSTS[spec['host']]
    frames = spec['frames']
    complete, end, manifest, review = [read(root / name) for name in ('complete.json', 'exit.json', 'manifest.json', 'semantic-review.json')]
    tail = complete.get('neutral_tail')
    if type(tail) is not int or not 0 <= tail <= 6000:
        raise ValueError('invalid declared source tail')
    endpoint = frames + tail
    if (complete.get('frame'), complete.get('original_inputs'), complete.get('full_movie')) != (endpoint, frames, True):
        raise ValueError('full original source movie required')
    if end.get('exit_code') != 0 or end.get('stop_reason') is not None or end.get('input_completion') is not True or end.get('original_movie_unchanged') is not True:
        raise ValueError('source did not complete cleanly with unchanged input')
    if role == 'observer' and end.get('stock_comparison') is not True:
        raise ValueError('observer pass did not reproduce the stock control')
    frame = review.get('frame')
    if review.get('completion_observed') is not True or type(frame) is not int or not frames <= frame <= endpoint or not review.get('witness', '').strip():
        raise ValueError('source completion has not been witnessed')
    if review.get('image') != f'frame-{frame:06d}.png' or frame not in captured_frames(endpoint, frames) or digest(root / review['image']) != review.get('image_sha256'):
        raise ValueError('source ending image differs')
    verify_identity(root, manifest, title, role, tail)
    terminal = return_rows(root / 'ram-frames.tsv', endpoint)
    evidence = [root / name for name in ['complete.json', 'exit.json', 'manifest.json', 'semantic-review.json',
                                         'ram-frames.tsv', 'loaded-bios.json', 'effective-sync.json',
                                         'effective-settings.json', 'effective-movie-policy.json', review['image']]]
    if host['has_run_policy']:
        evidence.append(root / 'effective-run-policy.json')
    if host['has_helper_build']:
        evidence.append(root / 'helper-build.json')
    if role == 'observer' or host['stock_loads_helper']:
        evidence.append(root / 'loaded-core.json')
    if role == 'observer':
        evidence.append(root / 'observer-build.json')
    return endpoint, terminal, evidence


def qualify(stock, observed, title, output):
    stock, observed = Path(stock).resolve(), Path(observed).resolve()
    if stock == observed:
        raise ValueError('stock and observer must be independent runs')
    output = Path(output)
    if output.exists():
        raise ValueError('never overwrite an admitted reference')
    spec = TITLES[title]
    frames = spec['frames']
    endpoint, terminal, evidence = admit(stock, title, 'stock')
    other, last, more = admit(observed, title, 'observer')
    if other != endpoint or last != terminal:
        raise ValueError('source terminal boundary differs')
    reference = next(b for b in read(observed / 'manifest.json')['bindings'] if Path(b['path']).name == 'ram-frames.tsv')
    if Path(reference['path']).resolve() != stock / 'ram-frames.tsv' or read(observed / 'stock-comparison.json').get('match') is not True:
        raise ValueError('observer was not compared against this stock control')
    evidence += more + compare_stock_observations(stock, observed, endpoint, frames)
    pages = observed / 'ram-pages.tsv'
    raw = observed / f'ram-frame-{endpoint:06d}.bin'
    terminal_consistency(pages, raw, endpoint, terminal[3])
    with pages.open() as stream:
        for line in stream:
            pass
    terminal_clock = int(line.split('\t')[1])
    for d in (stock, observed):
        evidence += [Path(b['path']) for b in read(d / 'manifest.json')['bindings']]
        evidence += [Path(e['path']) for e in read(d / 'host-closure.json')]
    evidence += [pages, raw, observed / 'stock-comparison.json']
    files = sorted({p.resolve(strict=True) for p in evidence})
    images = sum(1 for p in files if p.suffix == '.png' and p.parent == observed)
    key, bios_sha, _ = FIRMWARE[spec['firmware']]
    result = {'schema': spec['schema'], 'title': spec['title'], 'source_tag': spec['host'], 'source_qualification': 'pass',
              'movie_sha256': spec['movie_sha256'], 'bios_sha256': bios_sha, 'firmware_key': key,
              'observed_returns': endpoint, 'original_inputs': frames, 'neutral_tail': endpoint - frames,
              'terminal_clock': terminal_clock, 'terminal_ram_sha256': terminal[3].lower(),
              'ram_pages': str(pages), 'terminal_ram': str(raw), 'stock_source': str(stock), 'observer_source': str(observed),
              'images_equal': images, 'bindings': [{'path': str(f), 'sha256': digest(f)} for f in files],
              'admission_tool_sha256': digest(Path(__file__)),
              'scope': 'qualified rebuilt Octoshock %s observer and witnessed source endpoint; not whole-machine hardware accuracy' % spec['host']}
    output.write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('title', choices=sorted(TITLES))
    parser.add_argument('stock', type=Path)
    parser.add_argument('observed', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = qualify(args.stock, args.observed, args.title, args.output)
    print(json.dumps({'source_reference_admitted': True, 'title': args.title, 'returns': result['observed_returns'],
                      'images_equal': result['images_equal'], 'terminal_clock': result['terminal_clock']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
