"""Bind every launch to the binary that actually runs.

A title run may only execute the setup receipt's executable or a binary whose
SHA-256 was declared up front with --diagnostic-binary. A diagnostic run keeps
its full mechanical comparison but is never reported as a qualifying pass.
"""
import hashlib
import json
from pathlib import Path
import re

HEX64 = re.compile('[0-9a-f]{64}')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def resolve_binary(setup_executable, setup_sha256, override=None, diagnostic=None):
    """Return the identity fields for the executable that will run, or refuse.

    actual == setup receipt hash: qualifying. Otherwise the run is allowed only
    when --diagnostic-binary names exactly that hash; a path that silently still
    holds another binary is refused before anything launches.
    """
    if diagnostic is not None:
        diagnostic = str(diagnostic).lower()
        if not HEX64.fullmatch(diagnostic): raise ValueError('--diagnostic-binary must be a 64-digit hex SHA-256')
    if not isinstance(setup_sha256, str) or not HEX64.fullmatch(setup_sha256.lower()):
        raise ValueError('setup receipt has no valid executable_sha256')
    setup_sha256 = setup_sha256.lower()
    path = Path(override if override is not None else setup_executable).resolve(strict=True)
    actual = digest(path)
    matches = actual == setup_sha256
    if not matches and actual != diagnostic:
        message = f'binary differs from the setup receipt: {path} is {actual}, setup receipt executable_sha256 is {setup_sha256}'
        if diagnostic is not None: message += f', declared diagnostic binary is {diagnostic}'
        raise ValueError(message)
    return {'path': str(path), 'setup_executable_sha256': setup_sha256, 'binary_sha256': actual,
            'binary_matches_setup': matches, 'diagnostic_binary': diagnostic}


def receipt_fields(binary):
    return {key: binary[key] for key in ('setup_executable_sha256', 'binary_sha256', 'binary_matches_setup', 'diagnostic_binary')}


def add_launch_arguments(parser):
    """The shared run flags every title command accepts with the same spelling."""
    parser.add_argument('--exe', type=Path, help='executable to run instead of the setup receipt executable')
    parser.add_argument('--diagnostic-binary', metavar='SHA256',
                        help='declared SHA-256 of a non-setup binary; the run is recorded as diagnostic, never as a qualifying pass')
    parser.add_argument('--cpu-boundary-window', type=int, nargs=2, metavar=('LO', 'HI'),
                        help='forwarded unchanged to run_native --cpu-boundary-window')
    parser.add_argument('--stop-on-divergence', action='store_true',
                        help='let the streaming watcher stop the native process at the first divergence (the run then fails)')
    parser.add_argument('--returns', type=int, help='diagnostic prefix: compare this many returns (N < input count) or exactly the endpoint')
    parser.add_argument('--ladder', help='comma-separated increasing return counts, optionally ending in "full"; mutually exclusive with --returns')
    # Precise-slice speed variants, forwarded unchanged. Each defaults to the
    # qualified behaviour, so omitting them leaves the run exactly as before;
    # run_native records the resulting PSX_SLICE_* environment in the manifest,
    # so a receipt always names the variant that produced it.
    parser.add_argument('--slice-gpu-deadline', choices=('tick', 'phase'), default='tick',
                        help='precise-slice GPU deadline: the service clock tick (qualified) or the raster phase edge')
    parser.add_argument('--slice-bound', choices=('steal', 'nosteal', 'icache', 'none'), default='steal',
                        help='precise-slice block bound: 240 cycles per access (qualified), nosteal, icache, or no slicing')
    parser.add_argument('--deadline-cache', choices=('off', 'on'), default='off',
                        help='reuse device deadlines while the device-state generation is unchanged')
    parser.add_argument('--slot-take', choices=('off', 'on'), default='off',
                        help='take interrupts at compiled delay-slot boundaries the way exec_delay_slot does')
    # TAS checkpoints (docs/TAS_CHECKPOINTS.md). Capturing does not change what a run
    # qualifies; resuming always makes the run diagnostic.
    parser.add_argument('--save-state-at', type=int, nargs='+', metavar='RETURN',
                        help='save a full-machine checkpoint at each listed frontend return')
    parser.add_argument('--save-state-every', type=int, metavar='N',
                        help='save a checkpoint at every multiple of N returns inside the observed interval')
    parser.add_argument('--resume-from', type=Path, metavar='STATE',
                        help='diagnostic: resume from a checkpoint .pst; only the later returns are compared, never a pass')
    parser.add_argument('--resume-compatible-build', action='store_true',
                        help='with --resume-from: admit a rebuilt runtime with the same checkpoint compatibility identifier')
    return parser


def check_launch_arguments(args):
    if getattr(args, 'ladder', None) is not None and getattr(args, 'returns', None) is not None:
        raise ValueError('--ladder and --returns are mutually exclusive')
    if getattr(args, 'diagnostic_binary', None) is not None and not HEX64.fullmatch(str(args.diagnostic_binary).lower()):
        raise ValueError('--diagnostic-binary must be a 64-digit hex SHA-256')
    every = getattr(args, 'save_state_every', None)
    if every is not None and every < 1:
        raise ValueError('--save-state-every must be a positive return count')
    if any(frame < 1 for frame in getattr(args, 'save_state_at', None) or ()):
        raise ValueError('--save-state-at returns must be positive')
    resume = getattr(args, 'resume_from', None)
    if getattr(args, 'resume_compatible_build', False) and resume is None:
        raise ValueError('--resume-compatible-build requires --resume-from')
    if getattr(args, 'ladder', None) is not None and (
            resume is not None or every is not None or getattr(args, 'save_state_at', None)):
        raise ValueError('checkpoint capture and resume apply to one replay, not a ladder')
    if resume is not None:
        checkpoint_resume(args)


def checkpoint_resume(args):
    """(saved return, inputs consumed) of --resume-from, or (0, 0) for a cold run.

    The manifest beside the state is the runtime's own record; run_native and the
    runtime re-verify its identity, size and SHA-256 before anything loads."""
    path = getattr(args, 'resume_from', None)
    if path is None:
        return 0, 0
    manifest = json.loads(Path(str(path) + '.json').read_text())
    frame, consumed = manifest.get('frame'), manifest.get('input_consumed')
    if (manifest.get('schema') != 'psx-tas-stateio-v2' or type(frame) is not int or
            type(consumed) is not int or frame < 1 or consumed < 1):
        raise ValueError(f'not a TAS checkpoint manifest: {path}.json')
    return frame, consumed


def checkpoint_returns(args, observed):
    """The returns to capture: --save-state-at plus every --save-state-every multiple,
    all strictly after a resume point and no later than the observed endpoint."""
    start = checkpoint_resume(args)[0]
    explicit = set(getattr(args, 'save_state_at', None) or ())
    outside = sorted(frame for frame in explicit if not start < frame <= observed)
    if outside:
        raise ValueError(f'--save-state-at returns {outside} are outside the run ({start}, {observed}]')
    every = getattr(args, 'save_state_every', None)
    if every:
        explicit.update(range((start // every + 1) * every, observed + 1, every))
    return sorted(explicit)


def checkpoint_receipt(args, observed):
    """Receipt fields naming the checkpoints a run captured and the one it resumed from."""
    start, consumed = checkpoint_resume(args)
    resume = getattr(args, 'resume_from', None)
    return {'save_state_at': checkpoint_returns(args, observed),
            'resumed_from': str(Path(resume).resolve()) if resume is not None else None,
            'resumed_return': start, 'resumed_inputs': consumed,
            'resume_compatible_build': bool(getattr(args, 'resume_compatible_build', False))}


def run_native_arguments(args, binary, observed=None):
    """Extra run_native argv: bind the staged executable, forward the CPU window,
    any precise-slice variant that differs from the qualified default, and the
    checkpoint capture/resume options. `observed` is the run's last compared
    return; it bounds --save-state-every and is required when either is used."""
    argv = ['--expected-exe-sha256', binary['binary_sha256']]
    wants_checkpoints = getattr(args, 'save_state_at', None) or getattr(args, 'save_state_every', None)
    if wants_checkpoints or getattr(args, 'resume_from', None) is not None:
        if observed is None:
            raise ValueError('this title command does not bound checkpoint returns')
        returns = checkpoint_returns(args, observed)
        if returns:
            argv += ['--save-state-at', *map(str, returns)]
        if args.resume_from is not None:
            argv += ['--resume-from', str(Path(args.resume_from).resolve(strict=True))]
            if args.resume_compatible_build:
                argv.append('--resume-compatible-build')
    window = getattr(args, 'cpu_boundary_window', None)
    if window: argv += ['--cpu-boundary-window', str(window[0]), str(window[1])]
    for flag, attribute, default in (('--slice-gpu-deadline', 'slice_gpu_deadline', 'tick'),
                                     ('--slice-bound', 'slice_bound', 'steal'),
                                     ('--deadline-cache', 'deadline_cache', 'off'),
                                     ('--slot-take', 'slot_take', 'off')):
        value = getattr(args, attribute, default)
        if value != default: argv += [flag, value]
    return argv
