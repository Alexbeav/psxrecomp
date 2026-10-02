"""A ChangeThread called inside an interrupt handler switches at the call (PS1B-417).

--src-root and --control exist for the control run: they build the same guest
against the runtime sources of another tree (one from before the fix), where it
must fail on what the guest sees.
"""
import argparse
from pathlib import Path
import re
import tempfile
from source_fixture_link import build_and_run


def report_side(root):
    """The run report writes the counters that traps.c keeps (the report writer
    links into the game runtime only, so its wiring is checked in the source)."""
    report = (root / 'runtime/src/crash_trace.c').read_text(encoding='utf-8')
    call = report.index('psx_changethread_in_handler_stats(ct);')
    fmt = re.search(
        r'"  \\"changethread_in_handler\\": \{\\n"\s*'
        r'"    \\"switches\\": %u,\\n"\s*'
        r'"    \\"same_thread\\": %u,\\n"\s*'
        r'"    \\"first_epc\\": \\"0x%08X\\",\\n"\s*'
        r'"    \\"first_target\\": \\"0x%08X\\"\\n"\s*'
        r'"  \},\\n",\s*ct\[0\], ct\[1\], ct\[2\], ct\[3\]\);',
        report[call:])
    assert fmt, ('the report must write "changethread_in_handler": {"switches", '
                 '"same_thread", "first_epc", "first_target"} from the counters')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--src-root', default=None,
                        help='the runtime directory whose src/ and include/ are built')
    parser.add_argument('--control', action='store_true',
                        help='build without the counters (sources from before the fix)')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    src_root = Path(args.src_root).resolve() if args.src_root else here.parent
    if not args.control:
        report_side(src_root.parent)
    with tempfile.TemporaryDirectory() as root:
        guest = here
        if args.control:
            # The helper passes no define to the guest file, so the control
            # builds a copy that starts with it.
            guest = Path(root) / 'guest'
            guest.mkdir()
            (guest / 'rfe_guest_fixture.h').write_bytes((here / 'rfe_guest_fixture.h').read_bytes())
            (guest / 'test_changethread_in_handler.c').write_bytes(
                b'#define CHANGETHREAD_CONTROL 1\n'
                + (here / 'test_changethread_in_handler.c').read_bytes())
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, guest, src_root, opt, Path(root),
                          ['interrupts.c', 'traps.c', 'psx_bios_backend.c'],
                          'test_changethread_in_handler.c')
    print('PASS: a ChangeThread called inside a handler switches at the call (O0/O2)')
