"""Check core labels and the actual argument reader without launching a title."""
import argparse
import contextlib
import io
from unittest.mock import patch
from pathlib import Path
from types import SimpleNamespace

import biohazard
import megamanx4
import megamanx5
import redc
import run_native


class Parsed(Exception):
    def __init__(self, arguments):
        self.arguments = arguments


original_parse = argparse.ArgumentParser.parse_args


def stop_after_parse(parser, *args, **kwargs):
    raise Parsed(original_parse(parser, *args, **kwargs))


for module, version in ((megamanx4, '1.32.1'), (biohazard, '1.29.0'),
                        (megamanx5, '1.29.0'), (redc, '1.29.0')):
    for option, suffix in (('pad-ack', '-dualshock'), ('card', ''),
                           ('cd-drive', ''), ('mdec-source', '')):
        flag = '--' + option + '-model'
        assert module.PROFILE[module.PROFILE.index(flag) + 1] == 'nymashock-' + version + suffix

required = ['run', '--exe', 'exe', '--game', 'game', '--route', 'route',
            '--disc', 'disc', '--bios', 'bios']
for option, suffix in (('pad-ack', '-dualshock'), ('card', ''),
                       ('cd-drive', ''), ('mdec-source', '')):
    for version in ('1.29.0', '1.32.1', '9.9.9'):
        value = 'nymashock-' + version + suffix
        with patch('sys.argv', ['run_native.py', *required, '--' + option + '-model', value]), \
             patch.object(argparse.ArgumentParser, 'parse_args', stop_after_parse), \
             contextlib.redirect_stderr(io.StringIO()):
            try:
                run_native.main()
            except Parsed as parsed:
                assert version != '9.9.9'
                assert getattr(parsed.arguments, option.replace('-', '_') + '_model') == value
            except SystemExit as rejected:
                assert version == '9.9.9' and rejected.code == 2
            else:
                raise AssertionError('argument reader did not stop before file access')

# The 2.8 core's plain gamepad (PS1G-128): an explicit pad-ACK name and card label.
for option, value, valid in (('pad-ack', 'nymashock-2.8-gamepad', True),
                             ('pad-ack', 'nymashock-2.8-dualshock', False),
                             ('pad-ack', 'nymashock-2.8', False),
                             ('card', 'nymashock-2.8', True),
                             ('card', 'nymashock-2.8-gamepad', False)):
    with patch('sys.argv', ['run_native.py', *required, '--' + option + '-model', value]),          patch.object(argparse.ArgumentParser, 'parse_args', stop_after_parse),          contextlib.redirect_stderr(io.StringIO()):
        try:
            run_native.main()
        except Parsed as parsed:
            assert valid and getattr(parsed.arguments, option.replace('-', '_') + '_model') == value
        except SystemExit as rejected:
            assert not valid and rejected.code == 2
        else:
            raise AssertionError('argument reader did not stop before file access')

# A DualShock (PSXRTI2) route refuses the plain-gamepad ACK name; a digital route admits it.
for fmt, admitted in (('PSXRTI2', False), ('PSXRTI1', True)):
    class Reached(Exception):
        pass
    identity = {'frames': 2, 'format': fmt} if fmt == 'PSXRTI2' else {'frames': 2}
    with patch('sys.argv', ['run_native.py', *required, '--pad-ack-model', 'nymashock-2.8-gamepad']),          patch.object(Path, 'resolve', lambda self, strict=False: self),          patch.object(run_native, 'source_clock_identity', return_value=None),          patch.object(run_native, 'route_identity', return_value=identity),          patch.object(run_native, 'checkpoint_interval', side_effect=Reached):
        try:
            run_native.main()
        except ValueError as rejected:
            assert not admitted and 'plain-gamepad' in str(rejected), str(rejected)
        except Exception as other:
            assert admitted, repr(other)
        else:
            assert admitted

# Stop the real reader at card identity, after its selector/repair guard.
class CardAdmitted(Exception):
    pass


for version in ('1.29.0', '1.32.1', '2.8'):
    with patch('sys.argv', ['run_native.py', *required, '--card1', 'card',
                           '--card-model', 'nymashock-' + version,
                           '--legacy-card-repair', 'off']), \
         patch.object(Path, 'resolve', lambda self, strict=False: self), \
         patch.object(run_native, 'source_clock_identity', return_value=None), \
         patch.object(run_native, 'route_identity', return_value={'frames': 2}), \
         patch.object(run_native, 'card_identity', side_effect=CardAdmitted):
        try:
            run_native.main()
        except CardAdmitted:
            pass
        else:
            raise AssertionError('source card selector did not reach card identity')

# An old receipt reaches the existing exact-profile guard and must be refused.
old_profile = [value.replace('1.32.1', '1.29.0') for value in megamanx4.PROFILE]
receipt = {'schema': 'megamanx4-tas-candidate-v1', 'bindings': [],
           'executable': 'unused', 'executable_sha256': 'unused',
           'reference': 'unused', 'profile': old_profile}
args = SimpleNamespace(output=Path('unused'), setup=Path(__file__),
                       exe=None, diagnostic_binary=None)
with patch.object(megamanx4.source, 'read', return_value=receipt), \
     patch.object(megamanx4, 'require_hash'), \
     patch.object(megamanx4, 'resolve_binary', return_value=Path('unused')), \
     patch.object(megamanx4, 'verify_reference', return_value={'observed_returns': 1}):
    try:
        megamanx4.replay(args)
    except ValueError as rejected:
        assert str(rejected) == 'candidate profile differs; build a new candidate'
    else:
        raise AssertionError('old receipt was silently admitted under the renamed profile')

print('core selectors: four profile identities, eight valid labels, four unknown labels rejected; 2.8 plain-gamepad and card labels admitted, DualShock route refuses the gamepad ACK; card guards admit; old receipt refused')
