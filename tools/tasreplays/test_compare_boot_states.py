"""Independent raw/compressed checkpoint comparison and corruption checks."""
from pathlib import Path
import struct
import tempfile
import zlib
from compare_boot_states import compare, sections


def image(payload, compressed=False, tag=1):
    if compressed:
        payload = struct.pack('<I', len(payload)) + zlib.compress(payload)
    return struct.pack('<9I', 0x50535842, 8, 0, 0, 0, 0, 0, 1, 0) + struct.pack('<IIQ', tag, int(compressed), len(payload)) + payload


with tempfile.TemporaryDirectory() as root:
    a, b = [Path(root)/name for name in ('a.pst', 'b.pst')]
    a.write_bytes(image(b'cpu-state'))
    b.write_bytes(image(b'cpu-state', True))
    assert compare(a, b)['identical']
    b.write_bytes(image(b'cpu-State', True))
    assert compare(a, b)['differences'] == [dict(section='CPU', offset=4, left_bytes=9, right_bytes=9)]
    b.write_bytes(image(b'cpu-state', tag=23))
    assert len(compare(a, b)['differences']) == 2
    valid = image(b'cpu-state', True)
    for malformed in (valid[:-1], valid+b'junk', valid[:52]+struct.pack('<I', 1)+valid[56:]):
        b.write_bytes(malformed)
        try:
            sections(b)
        except ValueError:
            pass
        else:
            raise AssertionError('malformed checkpoint admitted')
print('Checkpoint comparator: compression-independent equality, byte offsets, missing sections and corruption pass')
