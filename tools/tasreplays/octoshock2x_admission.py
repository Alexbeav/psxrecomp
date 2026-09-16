"""Admit an independently replayed Octoshock 2.7 / 2.10 source pair, never native output.

Titles whose published movies were recorded on Octoshock cores newer than 2.3 (Crash Bandicoot
7798S on BizHawk 2.7, Abe's Exoddus 6672M on BizHawk 2.10) replay through
D:/psxrecomp/validation/tas/tools/source_control_octo2x.py, the descendant of the validated 2.3
launcher, with the ObservationOcto2x helper compiled per host and a rebuilt octoshock.dll carrying
the same passive cycle export as fork commit b3ec859c (2.3). This module is the Pepsiman admission
(pepsiman.py admit_source_control/verify_control_identity/reference) restated for that launcher's
manifest schema, with every host, helper, core, movie, media and firmware identity pinned per title.

Same rules: a source pass is admitted only with a witnessed semantic review naming one of its own
captured frames, a clean exit with the original movie unchanged, complete return coverage, and the
stock/observer pair byte-identical in RAM digests, lag, screenshots and effective identity. The
observer role is admissible only through that comparison because it runs a rebuilt core.
"""
from pathlib import Path
import argparse, hashlib, json, re, zipfile
from observation_evidence import captured_frames, compare_stock_observations, terminal_consistency

HOSTS = {
    '2.7': {'archive': 'BizHawk-2.7-win-x64.zip', 'archive_sha256': 'e7e69bbe10c52ef050ed0f4e09369c15c06fad8925073c1346b37dfe680e42b2',
            'emuhawk_sha256': '9cc462d0d0a3c42122ca8ee13f5665dbdecd41b8488ec1d7c3ebfedc288ab9ae',
            'stock_core_sha256': '2255db4c215f680257bc392e77c45f6cd160807a7ede37a6b8b43b38ac1ab3d0',
            'stock_commit': 'dbaf2595625f79093eeec37d2d4a7a9a4d37f370',
            'observer_core_sha256': '1d51965615a4d6bb223889fc1c78cda9643ddc917d0be8dc0d435d2b772c8c24',
            'observer_head': '2fe0a892e21d7b192a4b2bff73b77535ede7ced6',
            'helper_dll_sha256': '0a88a73726f22ae9a9f481261d0341ca2f47f90c9efe60384696e109d8fd249c'},
    '2.10': {'archive': 'BizHawk-2.10-win-x64.zip', 'archive_sha256': 'fdd0e7ae57afcb04509861408fdbb499bec52cffcc7b008526b60d3548a872ec',
             'emuhawk_sha256': 'a23eccb289d1a09b8b9ca09677718725acebab7d610e4b0ee93f7daf54064a25',
             'stock_core_sha256': '466a6a1f9d958a4d365b10fcf9b4bde5aeb80cc9c4d4fd00da9ad05930569b41',
             'stock_commit': 'dd232820493c05296c304b64bf09c57ff1e4812f',
             'observer_core_sha256': '5a02476a0b188a8f7e1e644196e7af25b4d1af3fa232022f6bb2ec61c58619e3',
             'observer_head': '72c1970f2de242e907756fc64c2f3500b66e71e8',
             'helper_dll_sha256': '646bd8ef541565518017f7940b3339e93d7685dc7c7625993d8a83bf463b46fe'},
}
HELPER_SOURCE_SHA = '3da48f1741f851261d0609d3873896bedad9a0390a7b6c0af2e08e4c792335d1'
LUA_SHA = '5bf2ea992918bba219b31932d2cf47212c113c497e72a23b0f8db6ef3364eedf'
# Launcher bytes bound into admitted runs. Two revisions are admissible: the storage budget was
# raised from 1 GiB to 6 GiB between the Crash control pass and its observer pass (the 1 GiB cap
# cut the first observer attempt at frame 120,733); nothing about observation changed.
LAUNCHER_SHAS = {
    'cd1bfc6f0023817214cb393ac9c029cc5bb675da538eb09df08f39c81df8c850': 'storage budget 1 GiB',
    '7df5adac5c4b9e1df0216c80bd395ab5c521acf7a51e6fea3225fd3f67f27368': 'storage budget 6 GiB',
}
FIRMWARE = {
    'SCPH5500.BIN': ('PSX+J', '9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb', 'b05def971d8ec59f346f2d9ac21fb742e3eb6917'),
    'SCPH5501.BIN': ('PSX+U', '11052b6499e466bbf0a709b1f9cb6834a9418e66680387912451e971cf8a1fef', '0555c6fae8906f3f09baf5988f00e55f88e9f30b'),
}
TITLES = {
    'crash-7798S': {
        'title': 'Crash Bandicoot (Japan) 100% 7798S', 'host': '2.7', 'frames': 203477,
        'movie': '100 CRASH.bk2', 'movie_sha256': 'ec7603aa532c68650d85f4a4d9d4e1cb0b1e1ea05d41ef3a45de4ef2dd88e92d',
        'firmware': 'SCPH5500.BIN',
        'media': {'Crash Bandicoot (Japan, Asia).bin': '2040778a42f55f06ae2bf3ce05ed8cf9602c49cb9b4eb4c5aad917a29eb31226',
                  'Crash Bandicoot (Japan, Asia).cue': '07d3591b5529af1f94d190e37a5c4830cb2ade96da5a8ec5fd33957367aac254'},
        'sync': {'EnableLEC': False, 'FIOConfig': {'Multitaps': [False, False], 'Memcards': [False, False], 'Devices8': [2, 0, 0, 0, 0, 0, 0, 0]}},
        'schema': 'crash-independent-source-v1'},
    'exoddus-6672M': {
        'title': "Oddworld Abe's Exoddus (USA) 100% 6672M", 'host': '2.10', 'frames': 478752,
        'movie': 'samtastic-oddworldabesexoddus-100p.bk2', 'movie_sha256': '5cabfa9b64b91ad93374bf1d28bbc129b05f6831e160c15de1e1c19f1aca55d7',
        'firmware': 'SCPH5501.BIN',
        # Two-disc set: the movie swaps discs twelve times (first at frame 179,996), so the host loads an
        # .m3u naming both cues; a Disc-1-only run crashed EmuHawk at the first swap (2026-09-16).
        'media': {"Oddworld - Abe's Exoddus (USA) (Disc 1).bin": 'd480e3aaf66a80c9307c1f2c61fefd162d2fb9e801398f69feb6e5bcbfb7c720',
                  "Oddworld - Abe's Exoddus (USA) (Disc 1).cue": '2c7ed2c4ad10825315a7bb763c6cf0c9daf6af9d4fdab2e5d27718fa8d35f96c',
                  "Oddworld - Abe's Exoddus (USA) (Disc 2).bin": 'a9db9be897ff2e5179943e460124a86a9df4a28a9a3a81fe19d1cfe01dacf007',
                  "Oddworld - Abe's Exoddus (USA) (Disc 2).cue": '47b55fa7294469cb90c704e22b7514a988f49f51bebf24e055b11489daba8dd2',
                  "Oddworld - Abe's Exoddus (USA).m3u": 'f91d5d417fd0e5da832a8116658192789bf033f21c5e2bfc0eceb8c80dcd09cf'},
        'sync': {'EnableLEC': False, 'FIOConfig': {'Multitaps': [False, False], 'Memcards': [False, False], 'Devices8': [1, 0, 0, 0, 1, 0, 0, 0]}},
        'schema': 'exoddus-independent-source-v1'},
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


def verify_identity(root, manifest, title, role, tail):
    spec = TITLES[title]
    host = HOSTS[spec['host']]
    key, bios_sha, bios_sha1 = FIRMWARE[spec['firmware']]
    frames = spec['frames']
    if (manifest.get('schema'), manifest.get('title'), manifest.get('role'), manifest.get('source_tag'),
        manifest.get('original_inputs'), manifest.get('cutoff'), manifest.get('neutral_tail'),
        manifest.get('firmware_key'), manifest.get('firmware_sha256')) != (
            'octoshock2x-stock-control-v1', spec['title'], role, spec['host'], frames, frames, tail, key, bios_sha):
        raise ValueError('source manifest role/host/firmware/input boundary differs')
    expected_commit = host['observer_head'] if role == 'observer' else host['stock_commit']
    if manifest.get('source_commit') != expected_commit:
        raise ValueError('source commit differs from the pinned %s revision' % role)
    if manifest.get('host_archive', {}).get('sha256') != host['archive_sha256']:
        raise ValueError('source host archive differs')
    bindings = manifest['bindings']
    names = {Path(b['path']).name: b for b in bindings}
    if len(names) != len(bindings):
        raise ValueError('ambiguous source binding names')
    for item in bindings:
        # The host rewrites config.ini on exit; the launcher froze the applied bytes as launch-config.json.
        target = root / 'launch-config.json' if Path(item['path']).name == 'config.ini' else Path(item['path'])
        if digest(target) != item['sha256']:
            raise ValueError('changed source artifact: ' + Path(item['path']).name)
    required = {*spec['media'], 'EmuHawk.exe', 'octoshock.dll', spec['movie'], 'original.bk2', spec['firmware'],
                'launch-config.json', 'start.lua', 'source_control_octo2x.lua', 'source_control_octo2x.py',
                'host-closure.json', 'ObservationOcto2x.dll', 'ObservationOcto2x.cs', 'helper-build.json'}
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
    if names['source_control_octo2x.lua']['sha256'] != LUA_SHA:
        raise ValueError('source observer lua differs')
    if names['source_control_octo2x.py']['sha256'] not in LAUNCHER_SHAS:
        raise ValueError('source launcher differs')
    if names['ObservationOcto2x.dll']['sha256'] != host['helper_dll_sha256'] or names['ObservationOcto2x.cs']['sha256'] != HELPER_SOURCE_SHA:
        raise ValueError('observer helper differs')
    for name in ('original.bk2', 'launch-config.json', 'start.lua', 'source_control_octo2x.lua', 'source_control_octo2x.py',
                 'host-closure.json', 'helper-build.json', 'observer-build.json'):
        if name in names and Path(names[name]['path']).resolve() != root / name:
            raise ValueError('run-local source artifact escapes: ' + name)
    helper = read(root / 'helper-build.json')
    if (helper.get('exit_code'), helper.get('host_tag'), helper.get('dll_sha256'), helper.get('source_sha256')) != (
            0, spec['host'], host['helper_dll_sha256'], HELPER_SOURCE_SHA):
        raise ValueError('observer helper build provenance differs')
    for entry in read(root / 'host-closure.json'):
        if digest(entry['path']) != entry['sha256']:
            raise ValueError('changed stock host closure: ' + entry['path'])
    core = names['octoshock.dll']
    loaded = read(root / 'loaded-core.json')
    if loaded.get('sha256') != core['sha256'] or Path(loaded['path']).resolve() != Path(core['path']).resolve():
        raise ValueError('loaded core differs from its bound binary')
    if loaded.get('type') != 'BizHawk.Emulation.Cores.Sony.PSX.Octoshock':
        raise ValueError('exact Octoshock core required')
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
            if (len(columns) != 4 or int(columns[0]) != expected or not re.fullmatch('[0-9A-Fa-f]{8}|[0-9A-Fa-f]{16}', columns[2])
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
    evidence = [root / name for name in ['complete.json', 'exit.json', 'manifest.json', 'semantic-review.json', 'ram-frames.tsv',
                                         'loaded-core.json', 'loaded-bios.json', 'effective-sync.json', 'effective-settings.json',
                                         'effective-movie-policy.json', 'effective-run-policy.json', 'helper-build.json', review['image']]]
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
        manifest = read(d / 'manifest.json')
        evidence += [d / 'launch-config.json' if Path(b['path']).name == 'config.ini' else Path(b['path']) for b in manifest['bindings']]
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
