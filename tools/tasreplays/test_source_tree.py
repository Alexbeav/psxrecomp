"""source_tree.py: the route setups' clean-source check leaves out generated/ only, and the
generated files a setup builds with are still measured.

The fault this covers (PS1B-109, replay pin 12): setup writes generated/<stem>.emitter.sha, and its
fingerprint hashes the project's bios.toml, which embeds absolute project paths. Every setup in a
new project directory rewrote that tracked stamp, so the plain `git status --porcelain` check
failed with "source changed during candidate build" (Pepsiman 6044 on Pegasus, replay pin 11).
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import source_tree  # noqa: E402

ROUTES = ('tekken3', 'pepsiman', 'crash', 'abesoddysee', 'biohazard', 'redc', 'megamanx4', 'megamanx5')
failures = []


def check(name, condition):
    print(('PASS ' if condition else 'FAIL ') + name, flush=True)
    if not condition:
        failures.append(name)


def git(root, *args):
    subprocess.run(['git', '-C', str(root), *args], check=True, capture_output=True,
                   encoding='utf-8', errors='replace')


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / 'src'
    project = Path(directory) / 'project'
    (root / 'generated').mkdir(parents=True)
    (root / 'bios').mkdir()
    (project / 'generated').mkdir(parents=True)
    (root / 'generated' / 'SCPH5500.emitter.sha').write_text('a' * 64 + '\n')
    (root / 'generated' / 'SCPH5500_full.c').write_text('/* old */\n')
    (root / 'bios' / 'SCPH5500.toml').write_text('rom = "bios/SCPH5500.BIN"\n')
    (project / 'generated' / 'GAME_full_0.c').write_text('/* game */\n')
    git(root, 'init', '-q')
    git(root, '-c', 'user.name=t', '-c', 'user.email=t@t', 'add', '-A')
    git(root, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'base')

    check('clean tree is clean', source_tree.dirty(root) == '')
    plain = lambda: subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain'],
                                            encoding='utf-8', errors='replace').strip()

    # The fault: setup rewrites the tracked stamp and BIOS C.
    (root / 'generated' / 'SCPH5500.emitter.sha').write_text('b' * 64 + '\n')
    (root / 'generated' / 'SCPH5500_full.c').write_text('/* regenerated */\n')
    check('before: the plain porcelain check fails on a rewritten generated/ stamp', plain() != '')
    check('after: a dirty tracked generated/ file passes', source_tree.dirty(root) == '')
    (root / 'generated' / 'SCPH5500_new.json').write_text('{}\n')
    check('after: an untracked generated/ file passes', source_tree.dirty(root) == '')

    record = source_tree.generated_record(root, 'SCPH5500', project)
    sha = lambda p: __import__('hashlib').sha256(p.read_bytes()).hexdigest()
    stamp = root / 'generated' / 'SCPH5500.emitter.sha'
    check('the dirty generated/ stamp is recorded with its new SHA-256',
          record.get(str(Path('generated') / 'SCPH5500.emitter.sha')) == sha(stamp) and sha(stamp) != __import__('hashlib').sha256(('a' * 64 + '\n').encode()).hexdigest())
    check('the regenerated BIOS C is recorded with its new SHA-256',
          record.get(str(Path('generated') / 'SCPH5500_full.c')) == sha(root / 'generated' / 'SCPH5500_full.c'))
    check('the project game C is recorded', record.get(str(project / 'generated' / 'GAME_full_0.c')) is not None)

    # Everything outside generated/ still fails.
    (root / 'bios' / 'SCPH5500.toml').write_text('rom = "elsewhere.BIN"\n')
    check('a dirty tracked file outside generated/ still fails', 'bios/SCPH5500.toml' in source_tree.dirty(root))
    git(root, 'checkout', '--', 'bios/SCPH5500.toml')
    (root / 'stray.c').write_text('int x;\n')
    check('an untracked file outside generated/ still fails', 'stray.c' in source_tree.dirty(root))
    (root / 'stray.c').unlink()
    (root / 'generatedx').mkdir()
    (root / 'generatedx' / 'f.c').write_text('int y;\n')
    check('a sibling path that only starts with "generated" still fails', 'generatedx' in source_tree.dirty(root))

# Every route setup uses the shared check in both of its paired places, and no plain porcelain
# check is left in a setup.
for name in ROUTES:
    text = (HERE / f'{name}.py').read_text(encoding='utf-8')
    check(f'{name}: both clean checks use source_tree.dirty', text.count('source_tree.dirty(ROOT)') == 2)
    check(f'{name}: no plain porcelain check left', not re.search(r"'status',\s*'--porcelain'\]", text))
    check(f'{name}: setup.json records the generated files', re.search(r"'generated'\s*:\s*(generated|source_tree\.generated_record)", text) is not None)

print(f'{len(failures)} failure(s)')
sys.exit(1 if failures else 0)
