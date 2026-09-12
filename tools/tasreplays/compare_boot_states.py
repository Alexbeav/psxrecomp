"""Compare independent checkpoint images, reporting uncompressed differences."""
import argparse
import json
from pathlib import Path
import struct
import zlib

NAMES = ['CPU', 'RAM', 'SPAD', 'IRQ', 'TIMER', 'CLOCK', 'GPU', 'VRAM',
         'SPU', 'SPURAM', 'CDROM', 'DMA', 'SIO', 'DIRTY', 'MDEC', 'ICACHE',
         'SCHED', 'RASTER', 'GPU_SERVICE', 'TIMER_SRC', 'DMA_SRC', 'IRQ_TIMING', 'CPU_EXEC']


def sections(path):
    data = Path(path).read_bytes()
    header = struct.unpack_from('<9I', data)
    if header[0] != 0x50535842:
        raise ValueError('invalid checkpoint magic')
    result, offset = {}, 36
    for _ in range(header[7]):
        tag, flags, size = struct.unpack_from('<IIQ', data, offset)
        offset += 16
        payload = data[offset:offset+size]
        if tag in result or flags not in (0, 1) or len(payload) != size:
            raise ValueError('duplicate, unsupported, or truncated section')
        offset += size
        if flags:
            length, = struct.unpack_from('<I', payload)
            decoder = zlib.decompressobj()
            raw = decoder.decompress(payload[4:], min(length, 64*1024*1024)+1)
            if len(raw) != length or not decoder.eof or decoder.unused_data:
                raise ValueError('invalid compressed section')
            payload = raw
        result[tag] = payload
    if offset != len(data):
        raise ValueError('trailing checkpoint data')
    return header, result


def compare(left, right):
    lh, ls = sections(left)
    rh, rs = sections(right)
    differences = []
    if lh != rh:
        differences.append({'section': 'HEADER', 'left': lh, 'right': rh})
    for tag in sorted(ls.keys() | rs.keys()):
        a, b = ls.get(tag, b''), rs.get(tag, b'')
        if a != b or (tag in ls) != (tag in rs):
            offset = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            differences.append({'section': NAMES[tag-1] if 1 <= tag <= len(NAMES) else hex(tag),
                                'offset': offset, 'left_bytes': len(a), 'right_bytes': len(b)})
    return {'identical': not differences, 'differences': differences}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('left', type=Path)
    parser.add_argument('right', type=Path)
    args = parser.parse_args()
    result = compare(args.left, args.right)
    print(json.dumps(result))
    raise SystemExit(0 if result['identical'] else 2)
