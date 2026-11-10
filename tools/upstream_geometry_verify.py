"""Compare the actual native reader with approved raw MODE2 payload bytes.

The derived cooked image preserves every 2048-byte sector's LBA. It is not
a replacement disc or evidence that cooked images preserve XA/raw behavior.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from upstream_consumer_verify import command, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempt', type=int, default=1)
    parser.add_argument('--no-op', action='store_true')
    args = parser.parse_args()
    if args.no_op:
        print('noop: native geometry client, approved local inputs, no game/window')
        return
    core = Path(__file__).resolve().parents[1]
    lane = core.parent
    pair = lane.parent / 'PS1B-165-pair'
    build = pair / 'rt'
    work = pair / f'geo-{args.attempt}'
    work.mkdir()
    config = json.loads((lane / 'reports/consumer-790-config.json').read_text(encoding='utf-8-sig'))
    gpp = Path(next(x.split('=', 1)[1] for x in config['configure_args']
                    if x.startswith('-DCMAKE_CXX_COMPILER=')))
    os.environ['PATH'] = str(gpp.parent) + os.pathsep + os.environ['PATH']
    ninja = lane / 'inputs/toolchain/bin/ninja.exe'
    probe = work / 'probe.exe'
    obj = work / 'probe.o'
    command([gpp, '-std=c++17', '-O1', '-I', core / 'runtime/include', '-c',
             core / 'tools/upstream_geometry_probe.cpp', '-o', obj], cwd=work)
    rule = 'CXX_EXECUTABLE_LINKER__iso_reader_geometry_test_Release'
    link = json.loads(command([ninja, '-t', 'compdb', '-x', rule], cwd=build))[0]['command']
    start = link.index(str(gpp)) + len(str(gpp))
    link = link[start:].split(' && ')[0].strip()
    original = 'CMakeFiles/iso_reader_geometry_test.dir/tests/test_iso_reader_geometry.cpp.obj'
    assert original in link or original.replace('/', '\\') in link
    link = link.replace(original, obj.as_posix()).replace(original.replace('/', '\\'), obj.as_posix())
    link = link.replace('iso_reader_geometry_test.exe', probe.as_posix())
    link = link.replace('libiso_reader_geometry_test.dll.a', (work / 'probe.dll.a').as_posix())
    sha_obj = build / 'CMakeFiles/psx-runtime.dir/src/psx_sha256.c.obj'
    assert sha_obj.is_file()
    response = work / 'link.rsp'
    response.write_text(link + ' ' + sha_obj.as_posix(), encoding='utf-8')
    command([gpp, '@' + str(response)], cwd=build)

    disc = pair / 'p/disc'
    raw = disc / 'Ape Escape (USA).bin'
    cue = disc / 'Ape Escape (USA).cue'
    expected_inputs = json.loads((lane / 'reports/paired-next-input-check.json').read_text(encoding='utf-8-sig'))
    for path in (raw, cue):
        expected = next(row for row in expected_inputs if Path(row['path']) == path)
        assert sha(path) == expected['sha256'], f'approved input differs: {path}'
    assert raw.stat().st_size % 2352 == 0
    count = raw.stat().st_size // 2352
    cooked = work / 'payload.iso'
    payload_hash = hashlib.sha256()
    raw_hash = hashlib.sha256()
    pvd = None
    with raw.open('rb') as source, cooked.open('xb') as dest:
        lba = 0
        while block := source.read(2352 * 128):
            raw_hash.update(block)
            for at in range(0, len(block), 2352):
                payload = block[at + 24:at + 24 + 2048]
                assert len(payload) == 2048
                if lba == 16:
                    pvd = payload
                payload_hash.update(payload)
                dest.write(payload)
                lba += 1
    assert lba == count and pvd is not None and pvd[1:6] == b'CD001'
    assert raw_hash.hexdigest() == next(row['sha256'] for row in expected_inputs if Path(row['path']) == raw)
    text = command([probe, cue, raw, cooked], cwd=work)
    (work / 'stdout.log').write_text(text, encoding='utf-8')
    geometries = []
    files = {}
    pvds = []
    for line in text.splitlines():
        if not line.startswith(('GEOMETRY ', 'PVD ', 'FILE ')):
            continue
        kind, *fields = line.split()
        row = dict(field.split('=', 1) for field in fields)
        if kind == 'GEOMETRY':
            geometries.append(row)
        elif kind == 'PVD':
            pvds.append(row)
        else:
            files.setdefault(row['name'], []).append(row)
    assert len(geometries) == len(pvds) == 3
    assert [row['raw'] for row in geometries] == ['1', '1', '0']
    for row in geometries:
        assert int(row['sectors']) == count and row['sha256'] == payload_hash.hexdigest()
    assert len({(row['root_lba'], row['root_bytes']) for row in geometries}) == 1
    for row in pvds:
        assert row['sha256'] == hashlib.sha256(pvd).hexdigest()
    for name in ('SYSTEM.CNF', 'SCUS_944.23'):
        expected = sha(disc / name)
        assert len(files[name]) == 3
        assert all(row['sha256'] == expected for row in files[name])
    result = dict(sectors=count, payload_bytes=count * 2048,
                  payload_sha256=payload_hash.hexdigest(), geometries=geometries,
                  pvds=pvds, files=files, probe_sha256=sha(probe), sha_object_sha256=sha(sha_obj),
                  original_bin_sha256=raw_hash.hexdigest(), probe_output=str(work / 'stdout.log'),
                  scope='actual single-track MODE2 data only; not XA/raw equivalence or gameplay')
    output = lane / f'reports/upstream-disc-geometry-{args.attempt}-result.json'
    assert not output.exists()
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
