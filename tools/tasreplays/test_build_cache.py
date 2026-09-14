"""Build cache: key canonicalisation, entry verification and copy-and-verify paths, without retail assets."""
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_cache as bc  # noqa: E402

ENV = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@x', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@x')
TOOLCHAIN = {'gcc': 'gcc (x) 16.1.0', 'gcc_machine': 'x86_64-w64-mingw32', 'g++': 'g++ (x) 16.1.0',
             'cmake': 'cmake version 3.30.0', 'ninja': '1.12.1', 'python': '3.12.0'}
K = 'a' * 64


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True, env=ENV).stdout.decode().strip()


def commit(repo, message):
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', message)
    return git(repo, 'rev-parse', 'HEAD')


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def rejects(call, exc=ValueError, text=None):
    try:
        call()
    except exc as error:
        if text and text not in str(error):
            raise AssertionError(f'{text!r} not in {error}')
        return
    raise AssertionError(f'expected {exc.__name__}')


def write(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_bytes(data.encode())
    return path


with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
    root = Path(directory).resolve()

    # ------------------------------------------------ tree/blob ids from a throwaway repository
    repo = root / 'repo'
    repo.mkdir()
    git(repo, 'init', '-q')
    for tree in set(bc.TOOLS_TREES + bc.NATIVE_TREES):
        write(repo / tree / 'a.txt', tree + '\n')
    write(repo / bc.TAS_CMAKE, 'cmake v1\n')
    write(repo / 'bios/SCPH1001.toml', 'rom = "bios/SCPH1001.BIN"\n')
    write(repo / 'recompiler/seeds.json', '{"seeds": 1}\n')
    write(repo / 'tools/tasreplays/tekken-seeds.txt', 'seed 1\n')
    head1 = commit(repo, 'one')
    trees1 = bc.tree_ids(repo, bc.TOOLS_TREES)
    assert list(trees1) == list(bc.TOOLS_TREES) and all(len(v) == 40 for v in trees1.values())
    blob1 = bc.blob_id(repo, bc.TAS_CMAKE)
    assert blob1 == bc.blob_id(repo, repo / bc.TAS_CMAKE) == git(repo, 'hash-object', bc.TAS_CMAKE)
    rejects(lambda: bc.tree_ids(repo, ['no-such-tree']), subprocess.CalledProcessError)
    # A runtime-only commit moves the runtime tree id and nothing else.
    write(repo / 'runtime/a.txt', 'runtime v2\n')
    head2 = commit(repo, 'two')
    trees2 = bc.tree_ids(repo, bc.TOOLS_TREES)
    assert trees2['runtime'] != trees1['runtime']
    assert all(trees2[t] == trees1[t] for t in bc.TOOLS_TREES if t != 'runtime')
    assert bc.blob_id(repo, bc.TAS_CMAKE) == blob1

    # ------------------------------------------------ cmake flag filtering
    tools_argv = ['cmake', '-S', repo / 'recompiler', '-B', root / 'tools', '-G', 'Ninja',
                  '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++',
                  '-DPSXRECOMP_ENABLE_CHD=ON', '-DBUILD_TESTING=ON', '-DPython3_EXECUTABLE=' + sys.executable]
    assert bc.cmake_key_args(tools_argv, bc.TOOLS_DROP) == [
        '-DBUILD_TESTING=ON', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=g++', '-DCMAKE_C_COMPILER=gcc',
        '-DPSXRECOMP_ENABLE_CHD=ON', '-GNinja']
    assert '-DPython3_EXECUTABLE=' + sys.executable in bc.cmake_key_args(tools_argv)

    def native_argv(project, bios_profile):
        return ['cmake', '-S', HERE, '-B', project / 'native', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                '-DTAS_PROJECT_DIR=' + str(project), '-DPSXRECOMP_BIOS_PROFILE=' + str(bios_profile),
                '-D_psxrt_bash=C:/Git/bin/bash.exe', '-DPSX_DEBUG_TOOLS=ON', '-DPython3_EXECUTABLE=x']
    filtered = bc.cmake_key_args(native_argv(root / 'p', root / 'p/bios.toml'), bc.NATIVE_DROP)
    assert filtered == ['-DCMAKE_BUILD_TYPE=Release', '-DPSX_DEBUG_TOOLS=ON', '-DPython3_EXECUTABLE=x', '-GNinja'], filtered
    assert filtered == bc.cmake_key_args(native_argv(root / 'elsewhere', root / 'elsewhere/b.toml'), bc.NATIVE_DROP)
    # Sorted: the same flags in another order key identically.
    shuffled = tools_argv[:5] + list(reversed(tools_argv[7:])) + tools_argv[5:7]
    assert bc.cmake_key_args(shuffled, bc.TOOLS_DROP) == bc.cmake_key_args(tools_argv, bc.TOOLS_DROP)

    # ------------------------------------------------ tools key: stable, and sensitive to each input
    doc = bc.tools_inputs(repo, tools_argv, TOOLCHAIN)
    assert set(doc) == {'trees', 'cmake_args', 'toolchain', 'source_date_epoch'} and doc['trees'] == trees2
    key_tools = bc.key_of(doc)
    assert key_tools == bc.tools_key(repo, tools_argv, TOOLCHAIN) == bc.tools_key(repo, shuffled, TOOLCHAIN)
    assert key_tools == bc.tools_key(repo, tools_argv[:-1] + ['-DPython3_EXECUTABLE=C:/other/python.exe'], TOOLCHAIN)
    assert key_tools != bc.tools_key(repo, tools_argv + ['-DBUILD_TESTING=OFF'], TOOLCHAIN)
    assert key_tools != bc.tools_key(repo, tools_argv, {**TOOLCHAIN, 'gcc': 'gcc (x) 15.2.0'})
    git(repo, 'checkout', '-q', head1)
    assert key_tools != bc.tools_key(repo, tools_argv, TOOLCHAIN)  # runtime-only commit invalidates tools
    git(repo, 'checkout', '-q', head2)
    assert key_tools == bc.tools_key(repo, tools_argv, TOOLCHAIN)
    assert bc.key_of({'b': 1, 'a': [1, 2]}) == sha(b'{"a":[1,2],"b":1}')

    # ------------------------------------------------ TOML normalisation across two project paths
    def project(name, census):
        p = root / name
        write(p / 'SLUS_000.01', b'boot program')
        write(p / 'SCPH1001.BIN', b'rom image')
        bios_seeds = repo / 'recompiler/seeds.json'
        q = lambda path: json.dumps(path.as_posix())
        write(p / 'bios.toml', f'[program]\nname = "Sony"\nrom = {q(p / "SCPH1001.BIN")}\nrom_lo = "0x1FC10000"\n'
                               f'[recompiler]\nseeds = {q(bios_seeds)}\nout_dir = {q(repo / "generated")}\nout_stem = "SCPH1001"\n')
        seeds = p / 'seeds.txt' if census else repo / 'tools/tasreplays/tekken-seeds.txt'
        write(p / 'game.toml', f'[game]\nname = "Title"\nexe = {q(p / "SLUS_000.01")}\n[recompiler]\nseeds = {q(seeds)}\n'
                               f'bios_config = {q(p / "bios.toml")}\nstrict = true\nout_dir = {q(p / "generated")}\n')
        return p

    A, B = project('proj-a', False), project('proj-b', False)
    text_a = bc.normalized_toml_text(A / 'game.toml')
    assert 'proj-a' not in text_a and 'proj-b' not in bc.normalized_toml_text(B / 'game.toml')
    assert f'exe = "{sha(b"boot program")}"' in text_a and 'out_dir = "out_dir"' in text_a and 'name = "Title"' in text_a
    assert bc.normalized_toml_sha(A / 'game.toml') == bc.normalized_toml_sha(B / 'game.toml')
    assert bc.normalized_toml_sha(A / 'bios.toml') == bc.normalized_toml_sha(B / 'bios.toml')
    bios_text = bc.normalized_toml_text(A / 'bios.toml')
    assert f'rom = "{sha(b"rom image")}"' in bios_text and 'rom_lo = "0x1FC10000"' in bios_text
    assert f'seeds = "{sha((repo / "recompiler/seeds.json").read_bytes())}"' in bios_text
    # bios_config nests: a ROM change in B shows through the game TOML key.
    write(B / 'SCPH1001.BIN', b'other rom')
    assert bc.normalized_toml_sha(A / 'game.toml') != bc.normalized_toml_sha(B / 'game.toml')
    write(B / 'SCPH1001.BIN', b'rom image')
    assert bc.normalized_toml_sha(A / 'game.toml') == bc.normalized_toml_sha(B / 'game.toml')
    # Line endings are canonical; a boot executable change is not.
    write(B / 'game.toml', (B / 'game.toml').read_bytes().replace(b'\n', b'\r\n'))
    assert bc.normalized_toml_sha(A / 'game.toml') == bc.normalized_toml_sha(B / 'game.toml')
    write(B / 'SLUS_000.01', b'boot program v2')
    assert bc.normalized_toml_sha(A / 'game.toml') != bc.normalized_toml_sha(B / 'game.toml')
    write(B / 'SLUS_000.01', b'boot program')
    # Census titles: the seeds file does not exist at key time and is keyed as the literal
    # "census"; the nested BIOS profile keeps hashing its own seeds.
    C = project('proj-c', True)
    census_keys = {**bc.PATH_KEYS, 'seeds': 'census'}
    rejects(lambda: bc.normalized_toml_text(C / 'game.toml'), FileNotFoundError)
    text_c = bc.normalized_toml_text(C / 'game.toml', census_keys)
    assert 'seeds = "census"' in text_c and 'proj-c' not in text_c
    assert f'bios_config = "{bc.normalized_toml_sha(C / "bios.toml")}"' in text_c

    # ------------------------------------------------ generated key
    tools = root / 'tools-a/build'
    for name in bc.TOOL_EXES:
        write(tools / name, b'tool ' + name.encode())
    def generated_doc(p, tools_dir=tools, seeds=repo / 'tools/tasreplays/tekken-seeds.txt', fingerprint='f' * 64):
        return bc.generated_inputs(tools_dir, 'SCPH1001', p / 'SCPH1001.BIN', repo / 'recompiler/seeds.json',
                                   git(repo, 'hash-object', 'bios/SCPH1001.toml'), fingerprint, p / 'SLUS_000.01', p / 'game.toml', seeds)
    gen_a = generated_doc(A)
    assert set(gen_a) == {'tools', 'bios', 'game'}
    assert bc.build_env()['SOURCE_DATE_EPOCH'] == bc.SOURCE_DATE_EPOCH and bc.SOURCE_DATE_EPOCH.isdigit()
    assert set(gen_a['tools']) == {'bios_exe_sha', 'game_exe_sha', 'toml_exe_sha'}
    assert set(gen_a['bios']) == {'stem', 'rom_sha', 'seeds_sha', 'profile_template_blob', 'emitter_fingerprint'}
    assert set(gen_a['game']) == {'boot_exe_sha', 'game_toml_normalized_sha', 'seeds'}
    assert gen_a['game']['seeds'] == sha(b'seed 1\n') and gen_a['bios']['rom_sha'] == sha(b'rom image')
    assert bc.key_of(gen_a) == bc.key_of(generated_doc(B))
    assert bc.key_of(gen_a) != bc.key_of(generated_doc(A, fingerprint='e' * 64))
    gen_c = generated_doc(C, seeds=None)
    assert gen_c['game']['seeds'] == 'census' and bc.key_of(gen_c) != bc.key_of(gen_a)
    write(tools / 'psxrecomp-game.exe', b'tool psxrecomp-game.exe v2')
    assert bc.key_of(generated_doc(A)) != bc.key_of(gen_a)
    write(tools / 'psxrecomp-game.exe', b'tool psxrecomp-game.exe')
    assert bc.key_of(generated_doc(A)) == bc.key_of(gen_a)

    # ------------------------------------------------ generated set and native key
    for p in (A, B):
        write(p / 'generated/SLUS_000.01_full_0.c', 'int game0;\n')
        write(p / 'generated/SLUS_000.01_dispatch.c', 'int dispatch;\n')
    rejects(lambda: bc.generated_set(repo, 'SCPH1001', A), ValueError, 'missing generated BIOS file')
    for name in bc.BIOS_GENERATED:
        write(repo / 'generated' / name.format(stem='SCPH1001'), 'bios ' + name + '\n')
    write(repo / 'generated/SCPH1001.emitter.sha', 'x' * 64 + '\n')  # stamp; never a native key input
    gs = bc.generated_set(repo, 'SCPH1001', A)
    assert list(gs) == ['bios/SCPH1001_full.c', 'bios/SCPH1001_dispatch.c', 'bios/SCPH1001_skipped_functions.json',
                        'game/SLUS_000.01_dispatch.c', 'game/SLUS_000.01_full_0.c'], list(gs)
    nat_a = bc.native_inputs(repo, gs, native_argv(A, A / 'bios.toml'), A / 'bios.toml', TOOLCHAIN)
    assert set(nat_a) == {'generated', 'trees', 'tas_cmake', 'cmake_args', 'bios_profile_normalized_sha', 'toolchain', 'source_date_epoch'}
    assert nat_a['tas_cmake'] == blob1 and list(nat_a['trees']) == list(bc.NATIVE_TREES)
    nat_b = bc.native_inputs(repo, bc.generated_set(repo, 'SCPH1001', B), native_argv(B, B / 'bios.toml'), B / 'bios.toml', TOOLCHAIN)
    assert bc.key_of(nat_a) == bc.key_of(nat_b)
    write(B / 'generated/SLUS_000.01_full_0.c', 'int game0_changed;\n')
    assert bc.key_of(nat_a) != bc.key_of(bc.native_inputs(repo, bc.generated_set(repo, 'SCPH1001', B), native_argv(B, B / 'bios.toml'), B / 'bios.toml', TOOLCHAIN))
    write(B / 'generated/SLUS_000.01_full_0.c', 'int game0;\n')
    # recompiler-only commit: tools key moves, native key does not; runtime-only: both move (shown above for tools).
    write(repo / 'recompiler/a.txt', 'recompiler v2\n')
    head3 = commit(repo, 'three')
    assert bc.tools_key(repo, tools_argv, TOOLCHAIN) != key_tools
    assert bc.key_of(bc.native_inputs(repo, gs, native_argv(A, A / 'bios.toml'), A / 'bios.toml', TOOLCHAIN)) == bc.key_of(nat_a)
    write(repo / 'runtime/a.txt', 'runtime v3\n')
    commit(repo, 'four')
    assert bc.key_of(bc.native_inputs(repo, gs, native_argv(A, A / 'bios.toml'), A / 'bios.toml', TOOLCHAIN)) != bc.key_of(nat_a)
    head = git(repo, 'rev-parse', 'HEAD')

    # ------------------------------------------------ store / lookup round trip
    cache = root / 'cache'
    log = write(A / 'generate-bios.log', 'bios log line 1\nline 2\n')
    receipt_in = {'inputs': gen_a, 'built_at': bc.now_iso(), 'source_head': head, 'project': str(A)}
    entry = bc.store(cache, 'generated', K, gs, {'generate-bios.log': log}, receipt_in)
    assert entry.path == cache / 'generated' / K and (entry.path / 'receipt.json').is_file()
    assert not list((cache / 'generated').glob('*.partial-*'))
    assert entry.receipt['schema'] == bc.SCHEMA and entry.receipt['stage'] == 'generated' and entry.receipt['key'] == K
    assert entry.receipt['files'] == {name: bc.digest(path) for name, path in gs.items()}
    assert entry.receipt['logs'] == {'generate-bios.log': bc.digest(log)} and entry.receipt['inputs'] == gen_a
    found = bc.lookup(cache, 'generated', K)
    assert found is not None and found.path == entry.path and found.receipt == json.loads((entry.path / 'receipt.json').read_text())
    assert bc.lookup(cache, 'generated', 'b' * 64) is None and bc.lookup(None, 'generated', K) is None
    assert bc.lookup(root / 'no-such-cache', 'generated', K) is None
    rejects(lambda: bc.lookup(cache, 'nope', K), ValueError)
    rejects(lambda: bc.lookup(cache, 'generated', '../x'), ValueError)

    # Partial entry: never read, even when complete inside.
    partial = cache / 'generated' / ('c' * 64 + '.partial-999')
    shutil.copytree(entry.path, partial)
    receipt = json.loads((partial / 'receipt.json').read_text()); receipt['key'] = 'c' * 64
    (partial / 'receipt.json').write_text(json.dumps(receipt))
    assert bc.lookup(cache, 'generated', 'c' * 64) is None
    assert [(s, p.name, st) for s, p, st, _ in bc.entries(cache) if st == 'partial'] == [('generated', partial.name, 'partial')]

    # Tampered entries are misses, not errors.
    target = entry.path / 'game/SLUS_000.01_full_0.c'
    original = target.read_bytes()
    target.write_bytes(b'int tampered;\n')
    assert bc.lookup(cache, 'generated', K) is None
    target.write_bytes(original)
    assert bc.lookup(cache, 'generated', K) is not None
    (entry.path / 'logs/generate-bios.log').write_text('edited log')
    assert bc.lookup(cache, 'generated', K) is None
    (entry.path / 'logs/generate-bios.log').write_bytes(log.read_bytes())
    assert bc.lookup(cache, 'generated', K) is not None
    target.unlink()
    assert bc.lookup(cache, 'generated', K) is None
    target.write_bytes(original)
    good = (entry.path / 'receipt.json').read_text()
    for bad in ['{not json', json.dumps({**json.loads(good), 'key': 'b' * 64}), json.dumps({**json.loads(good), 'stage': 'tools'}),
                json.dumps({**json.loads(good), 'schema': 'other'}), json.dumps({**json.loads(good), 'files': {}}),
                json.dumps({**json.loads(good), 'files': {'bios/SCPH1001_full.c': 'nothex'}}), '[]', '']:
        (entry.path / 'receipt.json').write_text(bad)
        assert bc.lookup(cache, 'generated', K) is None, bad[:30]
    (entry.path / 'receipt.json').write_text(good)
    assert bc.lookup(cache, 'generated', K) is not None
    assert [st for _, _, st, _ in bc.entries(cache)] == ['ok', 'partial']

    # ------------------------------------------------ copy_in verification and log provenance
    D = root / 'proj-d'
    targets = {name: (repo / 'generated-copy' / name[5:] if name.startswith('bios/') else D / 'generated' / name[5:]) for name in gs}
    bc.copy_in(found, targets)
    for name, path in gs.items():
        assert targets[name].read_bytes() == path.read_bytes()
    rejects(lambda: bc.copy_in(found, {'game/missing.c': D / 'x.c'}), KeyError)
    target.write_bytes(b'int tampered;\n')  # entry changed after lookup: the copy is rejected
    rejects(lambda: bc.copy_in(found, {'game/SLUS_000.01_full_0.c': D / 'y.c'}), RuntimeError, 'does not hash as its receipt')
    target.write_bytes(original)
    D.mkdir(exist_ok=True)
    written = bc.copy_logs(found, D)
    assert written == [D / 'generate-bios.log']
    lines = (D / 'generate-bios.log').read_bytes().split(b'\n', 1)
    assert lines[0].startswith(b'# build-cache hit:') and str(entry.path).encode() in lines[0] and head.encode() in lines[0]
    assert lines[1] == log.read_bytes()

    # ------------------------------------------------ race loser rule
    receipt_b = {**receipt_in, 'project': str(B), 'built_at': '2026-01-01T00:00:00+00:00'}
    partial_b = bc.new_partial(cache, 'generated', K)
    assert partial_b.name == f'{K}.partial-{os.getpid()}' and partial_b.is_dir()
    loser = bc.store(cache, 'generated', K, bc.generated_set(repo, 'SCPH1001', B), {'generate-bios.log': log}, receipt_b, partial=partial_b)
    assert loser.path == entry.path and loser.receipt['project'] == str(A) and not partial_b.exists()
    assert bc.lookup(cache, 'generated', K).receipt['project'] == str(A)
    # A complete-but-corrupt occupant is replaced by the fresh store.
    target.write_bytes(b'int tampered;\n')
    replaced = bc.store(cache, 'generated', K, bc.generated_set(repo, 'SCPH1001', B), {'generate-bios.log': log}, receipt_b)
    assert replaced.path == entry.path and replaced.receipt['project'] == str(B)
    assert bc.lookup(cache, 'generated', K).receipt['project'] == str(B)
    assert not list((cache / 'generated').glob(f'*.partial-{os.getpid()}'))

    # ------------------------------------------------ receipt block shape
    hit = bc.stage_record(K, True, replaced)
    assert hit == {'key': K, 'hit': True, 'entry': str(entry.path), 'built_at': '2026-01-01T00:00:00+00:00', 'source_head': head}
    fresh = bc.stage_record(None, False, None, 'now', head)
    assert fresh == {'key': None, 'hit': False, 'entry': None, 'built_at': 'now', 'source_head': head}
    block = bc.receipt_block(cache, fresh, hit, fresh)
    assert block == {'root': str(cache), 'tools': fresh, 'generated': hit, 'native': fresh}
    assert bc.receipt_block(None, fresh, fresh, fresh)['root'] is None
    json.dumps(block)

    # ------------------------------------------------ stage flows with synthetic builders
    calls = []

    def fake_tools(tools_dir):
        calls.append(('tools', tools_dir))
        for name in bc.TOOL_EXES:
            write(tools_dir / name, b'built ' + name.encode())
        for name in bc.TOOLS_LOGS:
            write(P / name, name + ' output\n')

    def tools_doc():
        return {'trees': trees2, 'cmake_args': bc.cmake_key_args(tools_argv, bc.TOOLS_DROP), 'toolchain': TOOLCHAIN, 'source_date_epoch': bc.SOURCE_DATE_EPOCH}

    P = root / 'stage-p'; P.mkdir()
    tools_dir, record = bc.stage_tools(None, repo, P, None, tools_doc, fake_tools, head)
    assert tools_dir == P / 'tools' and calls == [('tools', P / 'tools')] and record['key'] is None and record['hit'] is False
    explicit = root / 'explicit-tools'
    tools_dir, record = bc.stage_tools(cache, repo, P, explicit, tools_doc, fake_tools, head)
    assert tools_dir == explicit and calls[-1] == ('tools', explicit) and record['key'] is None and record['entry'] is None
    assert not (cache / 'tools').exists()
    tools_dir, record = bc.stage_tools(cache, repo, P, None, tools_doc, fake_tools, head)
    key = bc.key_of(tools_doc())
    assert tools_dir == cache / 'tools' / key / 'build'
    assert calls[-1][1] == cache / 'tools' / f'{key}.partial-{os.getpid()}' / 'build'  # built inside the partial entry
    assert '.partial-' in str(calls[-1][1]) and record == {'key': key, 'hit': False, 'entry': str(cache / 'tools' / key),
                                                          'built_at': record['built_at'], 'source_head': head}
    stored = bc.lookup(cache, 'tools', key)
    assert stored and set(stored.receipt['files']) == {f'build/{n}' for n in bc.TOOL_EXES} and set(stored.receipt['logs']) == set(bc.TOOLS_LOGS)
    assert stored.receipt['ctest_exit'] == 0 and stored.receipt['inputs'] == tools_doc() and stored.receipt['project'] == str(P)
    Q = root / 'stage-q'; Q.mkdir()
    count = len(calls)
    tools_dir2, record2 = bc.stage_tools(cache, repo, Q, None, tools_doc, fake_tools, head)
    assert tools_dir2 == tools_dir and len(calls) == count and record2['hit'] is True and record2['entry'] == str(cache / 'tools' / key)
    for name in bc.TOOLS_LOGS:
        assert (Q / name).read_text().splitlines()[0].startswith('# build-cache hit:') and (Q / name).read_text().endswith(name + ' output\n')
    # A failing build leaves neither an entry nor a partial.
    def failing(tools_dir):
        raise RuntimeError('ctest failed')
    other_doc = lambda: {**tools_doc(), 'toolchain': {**TOOLCHAIN, 'gcc': 'other'}}
    rejects(lambda: bc.stage_tools(cache, repo, Q, None, other_doc, failing, head), RuntimeError)
    assert bc.lookup(cache, 'tools', bc.key_of(other_doc())) is None and not list((cache / 'tools').glob('*.partial-*'))

    # Generated stage: miss on P writes the set; hit on Q copies it (with census outputs).
    gen_root = root / 'gen-repo'
    shutil.copytree(repo, gen_root)
    (gen_root / 'generated').mkdir(exist_ok=True)
    def fake_generate_for(project):
        def run():
            calls.append(('generate', project))
            write(project / 'census.toml', 'census\n'); write(project / 'seeds.txt', 'seeds\n'); write(project / 'census.log', 'census log\n')
            for name in bc.BIOS_GENERATED:
                write(gen_root / 'generated' / name.format(stem='SCPH5500'), 'gen ' + name + '\n')
            write(project / 'generated/SLPS_000.01_full_0.c', 'int g;\n'); write(project / 'generated/SLPS_000.01_dispatch.c', 'int d;\n')
            for name in bc.GENERATED_LOGS:
                write(project / name, name + '\n')
        return run
    gen_doc = lambda: {'tools': {'bios_exe_sha': '1'}, 'bios': {'stem': 'SCPH5500'}, 'game': {'seeds': 'census'}}
    record = bc.stage_generated(cache, gen_root, P, 'SCPH5500', gen_doc, True, fake_generate_for(P), head)
    gkey = bc.key_of(gen_doc())
    assert record['hit'] is False and record['key'] == gkey and calls[-1] == ('generate', P)
    stored = bc.lookup(cache, 'generated', gkey)
    assert set(stored.receipt['files']) == {'bios/SCPH5500_full.c', 'bios/SCPH5500_dispatch.c', 'bios/SCPH5500_skipped_functions.json',
                                            'game/SLPS_000.01_full_0.c', 'game/SLPS_000.01_dispatch.c', 'census.toml', 'seeds.txt'}
    assert set(stored.receipt['logs']) == {'census.log', 'generate-bios.log', 'generate-game.log'}
    for name in bc.BIOS_GENERATED:
        (gen_root / 'generated' / name.format(stem='SCPH5500')).write_text('stale from an earlier title\n')
    count = len(calls)
    record = bc.stage_generated(cache, gen_root, Q, 'SCPH5500', gen_doc, True, fake_generate_for(Q), head)
    assert record['hit'] is True and len(calls) == count
    assert (gen_root / 'generated/SCPH5500_full.c').read_text() == 'gen {stem}_full.c\n'
    assert (Q / 'generated/SLPS_000.01_full_0.c').read_text() == 'int g;\n' and (Q / 'census.toml').read_text() == 'census\n' and (Q / 'seeds.txt').read_text() == 'seeds\n'
    assert (Q / 'census.log').read_text().startswith('# build-cache hit:') and (Q / 'generate-game.log').read_text().endswith('generate-game.log\n')
    assert bc.stage_generated(None, gen_root, root / 'stage-r', 'SCPH5500', gen_doc, True, fake_generate_for(root / 'stage-r'), head)['key'] is None
    # Tekken-style (no census) entries carry no census files and no census log.
    tk_doc = lambda: {'game': {'seeds': 'tracked'}}
    def tk_generate():
        for name in bc.BIOS_GENERATED:
            write(gen_root / 'generated' / name.format(stem='SCPH1001'), 'tk ' + name + '\n')
        write(P / 'generated/SLUS_000.02_full_0.c', 'int t;\n')
        for name in bc.GENERATED_LOGS:
            write(P / name, name + '\n')
    bc.stage_generated(cache, gen_root, P, 'SCPH1001', tk_doc, False, tk_generate, head)
    tk = bc.lookup(cache, 'generated', bc.key_of(tk_doc()))
    assert 'census.toml' not in tk.receipt['files'] and set(tk.receipt['logs']) == set(bc.GENERATED_LOGS)

    # Native stage: miss builds; hit copies only the executable.
    def fake_native_for(project, native):
        def run():
            calls.append(('native', project))
            write(native / 'Title-TAS.exe', b'MZ player'); write(native / 'CMakeCache.txt', 'cache')
            for name in bc.NATIVE_LOGS:
                write(project / name, name + '\n')
        return run
    nat_doc = lambda: {'generated': {'a': '1'}, 'trees': {}, 'tas_cmake': blob1}
    record = bc.stage_native(cache, gen_root, P, P / 'native', 'Title-TAS', nat_doc, fake_native_for(P, P / 'native'), head)
    nkey = bc.key_of(nat_doc())
    assert record['hit'] is False and record['key'] == nkey and calls[-1] == ('native', P)
    stored = bc.lookup(cache, 'native', nkey)
    assert list(stored.receipt['files']) == ['Title-TAS.exe'] and set(stored.receipt['logs']) == set(bc.NATIVE_LOGS)
    count = len(calls)
    record = bc.stage_native(cache, gen_root, Q, Q / 'native', 'Title-TAS', nat_doc, fake_native_for(Q, Q / 'native'), head)
    assert record['hit'] is True and len(calls) == count and record['entry'] == str(cache / 'native' / nkey)
    assert (Q / 'native/Title-TAS.exe').read_bytes() == b'MZ player' and [p.name for p in (Q / 'native').iterdir()] == ['Title-TAS.exe']
    assert (Q / 'configure-native.log').read_text().startswith('# build-cache hit:')
    assert bc.stage_native(None, gen_root, root / 'stage-s', root / 'stage-s/native', 'Title-TAS', nat_doc,
                           fake_native_for(root / 'stage-s', root / 'stage-s/native'), head)['entry'] is None

    # ------------------------------------------------ list / prune
    out = io.StringIO()
    with redirect_stdout(out):
        assert bc.main(['--build-cache', str(cache), 'list']) == 0
    listing = out.getvalue()
    assert listing.splitlines()[0] == f'build cache: {cache}'
    assert f'tools     ok       {key}' in listing and f'native    ok       {nkey}' in listing and 'partial' in listing
    assert f'source_head={head}' in listing
    before = {p for _, p, _, _ in bc.entries(cache)}
    assert bc.prune(cache, 365) == [] and {p for _, p, _, _ in bc.entries(cache)} == before
    old = json.loads((cache / 'native' / nkey / 'receipt.json').read_text()); old['built_at'] = '2020-01-01T00:00:00+00:00'
    (cache / 'native' / nkey / 'receipt.json').write_text(json.dumps(old))
    stamp = time.time() - 400 * 86400
    os.utime(partial, (stamp, stamp))
    out = io.StringIO()
    with redirect_stdout(out):
        assert bc.main(['--build-cache', str(cache), 'prune', '--keep-days', '30']) == 0
    removed = out.getvalue().splitlines()
    # The race-test entry was stored with built_at 2026-01-01, so it is older than 30 days too.
    assert sorted(removed) == sorted([f'removed {cache / "native" / nkey}', f'removed {partial}', f'removed {cache / "generated" / K}']), removed
    assert not (cache / 'native' / nkey).exists() and not partial.exists() and bc.lookup(cache, 'tools', key) is not None
    assert bc.lookup(cache, 'generated', gkey) is not None
    out = io.StringIO()
    with redirect_stdout(out):
        assert bc.main(['--build-cache', str(root / 'empty-cache'), 'list']) == 0
    assert out.getvalue().splitlines()[-1] == '(empty)'

    # ------------------------------------------------ location resolution and setup flags
    import argparse
    parser = argparse.ArgumentParser(); bc.add_arguments(parser)
    args = parser.parse_args(['--build-cache', str(root / 'explicit')])
    assert bc.resolve_root(args) == (root / 'explicit').resolve()
    assert bc.resolve_root(parser.parse_args(['--no-build-cache'])) is None
    saved = os.environ.get(bc.ENV_VAR)
    try:
        os.environ[bc.ENV_VAR] = str(root / 'from-env')
        assert bc.resolve_root(parser.parse_args([])) == (root / 'from-env').resolve()
        assert bc.resolve_root(args) == (root / 'explicit').resolve()
        os.environ.pop(bc.ENV_VAR)
        default = bc.resolve_root(parser.parse_args([]))
        assert default.parts[-2:] == ('psxrecomp', 'build-cache')
        if os.environ.get('LOCALAPPDATA'):
            assert default == Path(os.environ['LOCALAPPDATA']) / 'psxrecomp' / 'build-cache'
    finally:
        if saved is not None:
            os.environ[bc.ENV_VAR] = saved

    # ------------------------------------------------ host-dependent identities (skipped when tools are absent)
    if all(shutil.which(tool) for tool in ('gcc', 'g++', 'cmake', 'ninja')):
        identity = bc.toolchain_identity()
        assert set(identity) == set(TOOLCHAIN) and identity['python'] == '%d.%d.%d' % sys.version_info[:3]
        assert bc.toolchain_identity() == identity
    else:
        print('toolchain_identity: skipped (gcc/g++/cmake/ninja not all on PATH)')
    # Same discovery as the setups (Git for Windows: <Git>/cmd/git.exe -> <Git>/bin/bash.exe); under a
    # Git Bash shell `git` resolves inside <Git>/mingw64, so also look one level higher.
    git_exe = shutil.which('git')
    candidates = [Path(git_exe).resolve().parents[n] / 'bin/bash.exe' for n in (1, 2)] if git_exe else []
    bash = next((c for c in candidates if c.is_file()), None)
    source_root = HERE.parent.parent
    if bash and bash.is_file() and (source_root / 'tools/bios_emitter_fingerprint.sh').is_file():
        fp_a = bc.portable_emitter_fingerprint(bash, source_root, A / 'bios.toml')
        fp_b = bc.portable_emitter_fingerprint(bash, source_root, B / 'bios.toml')
        assert fp_a == fp_b and len(fp_a) == 64, (fp_a, fp_b)
        write(B / 'bios.toml', (B / 'bios.toml').read_bytes() + b'extra = "value"\n')
        assert bc.portable_emitter_fingerprint(bash, source_root, B / 'bios.toml') != fp_a
    else:
        print('portable_emitter_fingerprint: skipped (Git for Windows bash not found)')

print('ok')
