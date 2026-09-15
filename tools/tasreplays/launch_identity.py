"""Bind every launch to the binary that actually runs.

A title run may only execute the setup receipt's executable or a binary whose
SHA-256 was declared up front with --diagnostic-binary. A diagnostic run keeps
its full mechanical comparison but is never reported as a qualifying pass.
"""
import hashlib
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
    return parser


def check_launch_arguments(args):
    if getattr(args, 'ladder', None) is not None and getattr(args, 'returns', None) is not None:
        raise ValueError('--ladder and --returns are mutually exclusive')
    if getattr(args, 'diagnostic_binary', None) is not None and not HEX64.fullmatch(str(args.diagnostic_binary).lower()):
        raise ValueError('--diagnostic-binary must be a 64-digit hex SHA-256')


def run_native_arguments(args, binary):
    """Extra run_native argv: bind the staged executable, forward the CPU window
    and any precise-slice variant that differs from the qualified default."""
    argv = ['--expected-exe-sha256', binary['binary_sha256']]
    window = getattr(args, 'cpu_boundary_window', None)
    if window: argv += ['--cpu-boundary-window', str(window[0]), str(window[1])]
    for flag, attribute, default in (('--slice-gpu-deadline', 'slice_gpu_deadline', 'tick'),
                                     ('--slice-bound', 'slice_bound', 'steal'),
                                     ('--deadline-cache', 'deadline_cache', 'off'),
                                     ('--slot-take', 'slot_take', 'off')):
        value = getattr(args, attribute, default)
        if value != default: argv += [flag, value]
    return argv
