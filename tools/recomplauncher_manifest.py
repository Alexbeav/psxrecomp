#!/usr/bin/env python3
"""Write recomplauncher.json for a title's setup package (PS1B-677).

RecompLauncher (recomplauncher.com) installs a recomp from its GitHub release
and reads `recomplauncher.json` at the top of the zip. This writes that file
from what the title already records, so no title keeps a hand-written copy:

  game.toml              [game] name, id, discs, disc_serials;
                         [prepare_disc] known_sizes, known_crc32;
                         [netplay] required_tracks
  catalog_identity.json  marketing.region (the Redump dump's name), optional

The file says:

  inputs   one input "game": the player's .cue or .chd, refused unless its
           serial is one of disc_serials. A one-track disc with a known CRC32
           also gets `dumps`, for the launcher's "verified" badge. Redump's
           CRC32 of a one-track disc is the CRC32 of its only track, which is
           what known_crc32 holds.
  prepare  the setup program with --prepare --disc <the player's copy>: it
           generates and builds the game without a window, printing
           `progress <0..1> <text>` lines. `done` is the built game, so the
           launcher skips the step once it is there.
  start    the setup program at the zip root, with --disc <the player's copy>;
           once built it forwards to the game.
  saves    the memory-card folder (runtime.memcard_dir, under the install).
  settings "Start in the recomp lobby" (A2): No passes --no-launcher and is
           the default.

The player's BIOS is not an input: the launcher has no BIOS support (its
schema says so). --prepare reads it from the shared BIOS folder every port
uses (runtime/include/psx_shared_bios.h), which the setup wizard fills.

Multi-disc titles are refused for now: the launcher wants one input per disc
(disc1..discN) and nothing here has been tried with it yet.

    recomplauncher_manifest.py write --root <title> --exe-name <name> \\
        --repo <owner/name> [--os windows|linux|macos] \\
        [--build-dir-name build-release] --out <stage>/recomplauncher.json
    recomplauncher_manifest.py check --manifest <file> --schema <schema.json>

`check` validates against the published schema
(https://recomplauncher.com/schema/recomplauncher.json, fetched by the
caller). It needs the `jsonschema` package; without it, it says so and exits 3.
"""
import argparse
import json
from pathlib import Path
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib  # type: ignore

SCHEMA_URL = 'https://recomplauncher.com/schema/recomplauncher.json'
DISC_EXTENSIONS = ['cue', 'chd']
EXIT_NO_VALIDATOR = 3
OSES = ('windows', 'linux', 'macos')


EXIT_UNSUPPORTED = 4


class ManifestError(Exception):
    pass


class Unsupported(ManifestError):
    """A title this tool does not write yet (exit 4; a packager notes it and goes on)."""


def _read_toml(path):
    try:
        return tomllib.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        raise ManifestError('%s not found' % path)
    except (tomllib.TOMLDecodeError, UnicodeError) as error:
        raise ManifestError('%s is not valid TOML: %s' % (path, error))


def _read_json(path):
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (ValueError, UnicodeError) as error:
        raise ManifestError('%s is not valid JSON: %s' % (path, error))


def _dumps(game_toml, catalog):
    """One Redump entry for a one-track disc whose CRC32 and size are known."""
    prep = game_toml.get('prepare_disc', {})
    crcs = prep.get('known_crc32') or []
    sizes = prep.get('known_sizes') or []
    tracks = game_toml.get('netplay', {}).get('required_tracks')
    counts = catalog.get('rom_identity', {}).get('track_counts')
    one_track = tracks == 1 or counts == [1]
    if not one_track or len(crcs) != 1:
        return []
    entry = {'name': catalog.get('marketing', {}).get('region') or 'Redump',
             'crc32': str(crcs[0]).lower()}
    if len(sizes) == 1:
        entry['size'] = int(sizes[0])
    return [entry]


def build_manifest(root, exe_name, repo, os_name='windows', build_dir_name='build-release'):
    root = Path(root)
    game_toml = _read_toml(root / 'game.toml')
    catalog = _read_json(root / 'catalog_identity.json')
    game = game_toml.get('game', {})
    name = game.get('name')
    if not name:
        raise ManifestError('[game] name missing from %s' % (root / 'game.toml'))
    discs = game.get('discs') or ([game['disc']] if game.get('disc') else [])
    if len(discs) > 1:
        raise Unsupported('%s has %d discs; multi-disc manifests are not written yet'
                            % (name, len(discs)))
    serials = game.get('disc_serials') or ([game['id']] if game.get('id') else [])
    if not serials:
        raise ManifestError('[game] disc_serials and id missing: the launcher cannot '
                            'tell this game from another')
    if not exe_name or exe_name.endswith('.exe'):
        raise ManifestError('--exe-name is the program name without .exe')
    if os_name not in OSES:
        raise ManifestError('--os is one of %s' % ', '.join(OSES))
    built = exe_name + ('.exe' if os_name == 'windows' else '')

    label = 'Your %s disc (%s) as .cue/.bin or .chd' % (name, ', '.join(serials))
    disc = {
        'id': 'game',
        'label': label,
        'role': 'game',
        'formats': [{'type': 'file', 'extensions': list(DISC_EXTENSIONS)}],
        'check': {'ids': list(serials)},
    }
    dumps = _dumps(game_toml, catalog)
    if dumps:
        disc['dumps'] = dumps

    memcards = game_toml.get('runtime', {}).get('memcard_dir') or 'saves'
    return {
        '$schema': SCHEMA_URL,
        'format': 1,
        'name': game_toml.get('runtime', {}).get('window_title') or name,
        'repo': repo,
        'inputs': [disc],
        'prepare': {
            'program': [exe_name + '.exe', exe_name],
            'args': ['--prepare', '--disc', '{input:game}'],
            'label': 'Building %s from your disc' % name,
            'done': ['{install}/%s/%s' % (build_dir_name, built)],
        },
        'start': {
            'program': [exe_name + '.exe', exe_name],
            'args': ['--disc', '{input:game}'],
        },
        'saves': ['{install}/%s' % memcards.strip('/\\')],
        'settings': [{
            'id': 'lobby',
            'label': 'Start in the recomp lobby',
            'in': 'args',
            'choices': [{'label': 'No', 'args': ['--no-launcher']},
                        {'label': 'Yes', 'args': []}],
        }],
    }


def write(args):
    manifest = build_manifest(args.root, args.exe_name, args.repo, args.os,
                              args.build_dir_name)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    print('wrote %s' % out)
    return 0


def check(args):
    try:
        import jsonschema
    except ModuleNotFoundError:
        print('skipped: the jsonschema package is not installed; %s was not validated'
              % args.manifest)
        return EXIT_NO_VALIDATOR
    manifest = _read_json(Path(args.manifest))
    schema = _read_json(Path(args.schema))
    if not manifest or not schema:
        raise ManifestError('manifest or schema missing or empty')
    validator = jsonschema.Draft7Validator(schema)
    errors = sorted(validator.iter_errors(manifest), key=lambda e: list(e.path))
    for error in errors:
        print('%s: %s' % ('/'.join(str(p) for p in error.path) or '(top)', error.message))
    print('%s: %d schema errors' % (args.manifest, len(errors)))
    return 1 if errors else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    w = sub.add_parser('write')
    w.add_argument('--root', required=True)
    w.add_argument('--exe-name', required=True)
    w.add_argument('--repo', required=True)
    w.add_argument('--os', default='windows', choices=OSES,
                   help='the system the zip is for; names the built game in prepare.done')
    w.add_argument('--build-dir-name', default='build-release')
    w.add_argument('--out', required=True)
    c = sub.add_parser('check')
    c.add_argument('--manifest', required=True)
    c.add_argument('--schema', required=True)
    args = parser.parse_args(argv)
    try:
        return write(args) if args.command == 'write' else check(args)
    except Unsupported as error:
        print('not written: %s' % error, file=sys.stderr)
        return EXIT_UNSUPPORTED
    except ManifestError as error:
        print('error: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
