"""Raw opcode partitions authored from field widths in primary hardware docs."""
import random

def words(kind, seed=172092003):
    if kind=='selectors':
        for primary in range(64):
            for function in range(64):
                for rs,rt,rd,shift in [(0,0,0,0),(3,5,7,11),(31,31,31,31)]:
                    yield primary<<26 | rs<<21 | rt<<16 | rd<<11 | shift<<6 | function
    elif kind=='registers':
        for primary in range(64):
            for function in range(64):
                for field in [21,16,11,6]:
                    for value in range(32):
                        base=primary<<26 | 3<<21 | 5<<16 | 7<<11 | 11<<6 | function
                        yield (base & ~(31<<field)) | value<<field
    elif kind=='holdout':
        rng=random.Random(seed)
        for _ in range(262144):yield rng.getrandbits(32)
    else:raise ValueError(kind)
