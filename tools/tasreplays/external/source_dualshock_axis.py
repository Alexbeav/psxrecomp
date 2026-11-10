# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the Nymashock DualShock stick-axis conversion as a lookup table.

Source-derived arithmetic: InputDevice_DualShock::UpdateInput in the
GPL-2.0-or-later source at
https://github.com/TASEmulators/mednafen/blob/52c06fc2cfc1f7f0c9d3a5fcbcac3216e40384ca/src/psx/input/dualshock.cpp
which converts each little-endian u16 axis the frontend supplies into the byte
the emulated pad reports. BizHawk pins that file per release as its
waterbox/nyma/mednafen submodule; commit 52c06fc2 is BizHawk 2.8 and 382ff1b8
is BizHawk 2.10, and the two files are byte-identical.

This separate source tool reads no movie, commands, emulator or game state: it
writes one fixed 65,536-byte table, index = the u16 the frontend supplies,
value = the byte the pad receives. The route encoder consumes that table as
data and contains neither this arithmetic nor a copy of it. No framework
license or redistribution suitability is asserted by this tool.
"""
import argparse
import hashlib
import json
from pathlib import Path

ENTRIES = 1 << 16


def table():
    """The 65,536 pad bytes, in frontend-value order."""
    return bytes((value * 255 + 32767) // 65535 for value in range(ENTRIES))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    data = table()
    assert len(data) == ENTRIES
    # Fixed points: neutral, both ends of the range, and monotonicity.
    assert data[0x8000] == 128 and data[0] == 0 and data[0xFFFF] == 255
    assert all(data[i] <= data[i + 1] for i in range(ENTRIES - 1))
    # The 2.9.1+ frontend spells a stick byte v8 as v8 << 8, which does NOT round
    # trip: 255 << 8 converts to 254, and the same off-by-one appears from 0x81
    # upward. run_native.route_identity models exactly this when it reports the
    # protocol bytes for a PSXRTI2 route, so the two agree by construction.
    assert data[255 << 8] == 254 and data[128 << 8] == 128
    with args.output.open('xb') as stream:
        stream.write(data)
    receipt = {'schema': 'nymashock-dualshock-axis-table-v1', 'entries': ENTRIES,
               'sha256': hashlib.sha256(data).hexdigest(),
               'source': 'InputDevice_DualShock::UpdateInput, TASEmulators/mednafen src/psx/input/dualshock.cpp',
               'commits': {'bizhawk-2.8': '52c06fc2cfc1f7f0c9d3a5fcbcac3216e40384ca',
                           'bizhawk-2.10': '382ff1b8d293c9a862497706808cbb79b2cecbfb'},
               'identical_between_those_commits': True,
               'expression': '(value * 255 + 32767) / 65535',
               'license': 'GPL-2.0-or-later; see COPYING beside this tool'}
    if args.receipt:
        with args.receipt.open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, indent=2)
            stream.write('\n')
    print(json.dumps({k: receipt[k] for k in ('entries', 'sha256', 'expression')}))


if __name__ == '__main__':
    main()
