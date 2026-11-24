#!/usr/bin/env python3
"""Write recomplauncher.json for a title's setup package (PS1B-677).

RecompLauncher (recomplauncher.com) installs a recomp from its GitHub release
and reads `recomplauncher.json` at the top of the zip. This writes that file
from what the title already records, so no title keeps a hand-written copy:

  game.toml  [game] name, id, discs, disc_serials, program_discs;
             [runtime] window_title, memcard_dir

The file says:

  inputs   one disc: input "game", the player's .cue or .chd, refused unless
           its serial is one of disc_serials. Several discs: inputs disc1 ..
           discN with role "disc", each checked against its own serial when
           disc_serials has one per disc, else against all of them. The first
           disc's label names .cue, so the launcher hands the game the .cue
           and not its first track.
  prepare  the setup program with --prepare and one --disc per disc: it
           records the set, generates and builds the game without a window,
           printing `progress <0..1> <text>` lines. `done` is the built game,
           so the launcher skips the step once it is there.
  start    the setup program at the zip root, with --disc <the boot disc>;
           once built it forwards to the game.
  saves    the memory-card folder (runtime.memcard_dir, under the install).
  settings "Start in the recomp lobby" (A2): start always passes
           --no-launcher and Yes adds --launcher, which wins in the runtime.
           The launcher adds nothing for a setting the player never clicked,
           so every first choice passes nothing. Video rows (renderer, display, vsync, antialiasing,
           dithering) pass the runtime's own one-run switches (--renderer,
           --fullscreen, --windowed, --set video.<key>=<value>); their first
           choice passes nothing and keeps what the player set in the game.

No `dumps`: the launcher compares the CRC32 of the very file the player picks,
which for a PS1 disc is the .cue sheet's text or the .chd container, never the
tracks Redump lists. A Redump CRC32 there would tell every good copy that it
"isn't a known clean dump". This waits on the launcher reading a sheet's tracks.

The player's BIOS is not an input: the launcher has no BIOS support (its
schema says so). --prepare reads it from the shared BIOS folder every port
uses (runtime/include/psx_shared_bios.h), which the setup wizard fills.

A set whose discs boot different programs ([game] program_discs) is refused
for now: each program is its own build.

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


MAX_DISCS = 8  # PSX_HOST_MAX_DISCS in host/psxrecomp_codegen_host.c


def _choices(*rows):
    """Setting choices from (label, args) pairs."""
    return [{'label': label, 'args': list(args)} for label, args in rows]


def _set(key, value):
    return ['--set', 'video.%s=%s' % (key, value)]


GAME_CHOICE = ("Game's setting", [])

VIDEO_SETTINGS = [
    {'id': 'renderer', 'label': 'Renderer', 'group': 'Video',
     'help': 'How the game draws. Vulkan is the fastest on most PCs.',
     'choices': _choices(GAME_CHOICE, ('Vulkan', ['--renderer', 'vulkan']),
                         ('OpenGL', ['--renderer', 'opengl']),
                         ('Software', ['--renderer', 'software']))},
    {'id': 'display', 'label': 'Display', 'group': 'Video',
     'choices': _choices(GAME_CHOICE, ('Fullscreen', ['--fullscreen']),
                         ('Exclusive fullscreen', _set('fullscreen', '2')),
                         ('Window', ['--windowed']))},
    {'id': 'vsync', 'label': 'VSync', 'group': 'Video',
     'choices': _choices(GAME_CHOICE, ('On', _set('vsync', 'on')),
                         ('Off', _set('vsync', 'immediate')),
                         ('Adaptive', _set('vsync', 'adaptive')),
                         ('Variable refresh (VRR)', _set('vsync', 'vrr')))},
    {'id': 'antialiasing', 'label': 'Antialiasing', 'group': 'Video',
     'choices': _choices(GAME_CHOICE, ('Off', _set('antialiasing_mode', 'off')),
                         ('FXAA', _set('antialiasing_mode', 'fxaa')),
                         ('FXAA (high quality)', _set('antialiasing_mode', 'fxaa_hq')))},
    {'id': 'dithering', 'label': 'Dithering', 'group': 'Video',
     'help': "The console's colour dithering. Fine keeps it at higher resolutions.",
     'choices': _choices(GAME_CHOICE, ('Off', _set('dithering', 'off')),
                         ('On', _set('dithering', 'on')),
                         ('Fine', _set('dithering', 'fine')))},
]


def _disc_inputs(name, discs, serials):
    """The player's disc: input "game", or disc1..discN for a set."""
    formats = [{'type': 'file', 'extensions': list(DISC_EXTENSIONS)}]
    if len(discs) <= 1:
        return [{
            'id': 'game',
            'label': 'Your %s disc (%s) as .cue/.bin or .chd' % (name, ', '.join(serials)),
            'role': 'game',
            'formats': formats,
            'check': {'ids': list(serials)},
        }]
    per_disc = len(serials) == len(discs)
    inputs = []
    for n in range(1, len(discs) + 1):
        ids = [serials[n - 1]] if per_disc else list(serials)
        inputs.append({
            'id': 'disc%d' % n,
            'label': 'Disc %d (%s) as .cue/.bin or .chd' % (n, ', '.join(ids)),
            'role': 'disc',
            'formats': formats,
            'check': {'ids': ids},
        })
    return inputs


def build_manifest(root, exe_name, repo, os_name='windows', build_dir_name='build-release'):
    root = Path(root)
    game_toml = _read_toml(root / 'game.toml')
    game = game_toml.get('game', {})
    name = game.get('name')
    if not name:
        raise ManifestError('[game] name missing from %s' % (root / 'game.toml'))
    discs = game.get('discs') or ([game['disc']] if game.get('disc') else [])
    if len(discs) > 1 and game.get('program_discs'):
        raise Unsupported('%s is a set of separate programs ([game] program_discs); '
                          'not written yet' % name)
    if len(discs) > MAX_DISCS:
        raise ManifestError('%s has %d discs; the setup program takes at most %d'
                            % (name, len(discs), MAX_DISCS))
    serials = game.get('disc_serials') or ([game['id']] if game.get('id') else [])
    if not serials:
        raise ManifestError('[game] disc_serials and id missing: the launcher cannot '
                            'tell this game from another')
    if not exe_name or exe_name.endswith('.exe'):
        raise ManifestError('--exe-name is the program name without .exe')
    if os_name not in OSES:
        raise ManifestError('--os is one of %s' % ', '.join(OSES))
    built = exe_name + ('.exe' if os_name == 'windows' else '')

    inputs = _disc_inputs(name, discs, serials)
    disc_args = []
    for disc in inputs:
        disc_args += ['--disc', '{input:%s}' % disc['id']]

    memcards = game_toml.get('runtime', {}).get('memcard_dir') or 'saves'
    return {
        '$schema': SCHEMA_URL,
        'format': 1,
        'name': game_toml.get('runtime', {}).get('window_title') or name,
        'repo': repo,
        'inputs': inputs,
        'prepare': {
            'program': [exe_name + '.exe', exe_name],
            'args': ['--prepare'] + disc_args,
            'label': 'Building %s from your disc' % name,
            'done': ['{install}/%s/%s' % (build_dir_name, built)],
        },
        'start': {
            'program': [exe_name + '.exe', exe_name],
            'args': ['--no-launcher'] + disc_args[:2],
        },
        'saves': ['{install}/%s' % memcards.strip('/\\')],
        'settings': [{
            'id': 'lobby',
            'label': 'Start in the recomp lobby',
            'in': 'args',
            'choices': [{'label': 'No', 'args': []},
                        {'label': 'Yes', 'args': ['--launcher']}],
        }] + [dict(row, **{'in': 'args'}) for row in VIDEO_SETTINGS],
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
