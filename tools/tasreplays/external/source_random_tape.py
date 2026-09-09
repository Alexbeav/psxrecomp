# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the fixed Octoshock 2.2.2 cold-reset raw generator sequence.

Source-derived arithmetic: MDFN_PseudoRNG in the GPL-2.0-or-later source at
https://github.com/TASEmulators/BizHawk/blob/2.2.2/psx/octoshock/psx/psx.cpp
This separate source tool reads no movie, commands, emulator or game state.
The native generic tape reader contains neither this generator nor its seed.
No framework license or redistribution suitability is asserted by this tool.
"""
import argparse,hashlib,json,struct
from pathlib import Path


def words(count):
    x,y,z,carry=123456789,987654321,43219876,6543217
    accumulator=0xDEADBEEFCAFEBABE
    mask32=(1<<32)-1
    for _ in range(count):
        x=(314527869*x+1234567)&mask32
        y=(y^(y<<5))&mask32
        y^=y>>7
        y=(y^(y<<22))&mask32
        product=4294584393*z+carry
        carry,z=product>>32,product&mask32
        accumulator=(19073486328125*accumulator+1)&((1<<64)-1)
        yield ((x+y+z)^(accumulator>>16))&mask32


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--count',type=int,default=65536)
    args=parser.parse_args()
    if not 1<=args.count<=1048576:raise ValueError('count outside 1..1048576')
    payload=b'PSX-CD-RNG1\0\0\0\0\0'+struct.pack('<I',args.count)
    payload+=b''.join(struct.pack('<I',word) for word in words(args.count))
    with args.output.open('xb') as file:file.write(payload)
    print(json.dumps({'count':args.count,'sha256':hashlib.sha256(payload).hexdigest(),
                     'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                     'source_identity':'BizHawk 2.2.2 / 519e14aa1ad7a9d6df2edc7808c5ed687dfee046',
                     'scope':'fixed raw cold-reset sequence; no command or movie inputs'}))


if __name__=='__main__':main()
