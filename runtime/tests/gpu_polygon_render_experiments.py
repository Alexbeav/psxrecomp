"""Synthetic caller fixtures independent of existing renderer tests."""
import itertools
import json
import random


def matrix():
    cases = []
    def add(vertices, **options):
        row = dict(id=f"render_{len(cases)}", seed=0, x=[p[0] for p in vertices],
                   y=[p[1] for p in vertices], clip=[0, 0, 1023, 511], offset=[0, 0],
                   blend_enabled=0, blend_mode=0, mask_set=0, mask_check=0,
                   colors=[0x2040ff, 0xf08010, 0x20ff40], shaded=0, dither=0,
                   interlace=0, skip_field=0, texture=None)
        row.update(options)
        cases.append(row)
    triangles = [[(4, 4), (31, 7), (9, 29)], [(0, 0), (1, 31), (2, 0)],
                 [(-8, -3), (17, 3), (2, 19)], [(12, 12), (20, 20), (28, 28)]]
    for triangle in triangles:
        for order in itertools.permutations(range(3)):
            for shaded, dither in itertools.product(range(2), repeat=2):
                add([triangle[i] for i in order], shaded=shaded, dither=dither)
    rng = random.Random(1723011)
    for i in range(160):
        vertices = [(rng.randrange(-8, 41), rng.randrange(-8, 41)) for _ in range(3)]
        add(vertices, seed=rng.choice([0, 1, 0xffffffff, 172]),
            clip=rng.choice([[0, 0, 1023, 511], [7, 3, 19, 27], [19, 27, 7, 3]]),
            offset=rng.choice([[0, 0], [7, 11], [-9, -13]]),
            blend_enabled=i % 2, blend_mode=i % 4, mask_set=(i // 2) % 2,
            mask_check=(i // 4) % 2, shaded=(i // 8) % 2, dither=(i // 16) % 2,
            interlace=(i // 32) % 2, skip_field=(i // 64) % 2)
    # UV footprints deliberately overlap the destination to expose visit order.
    for depth, raw, blend, shaded, order in itertools.product(range(3), range(2), range(2), range(2), range(6)):
        triangle = [(0, 0), (31, 4), (4, 31)]
        permutation = list(itertools.permutations(range(3)))[order]
        vertices = [triangle[j] for j in permutation]
        uv = [0, 31 | (4 << 8), 4 | (31 << 8)]
        add(vertices, seed=172, shaded=shaded, blend_enabled=blend, blend_mode=order % 4,
            texture=dict(uv=[uv[j] for j in permutation], window=0, page=depth << 7,
                         clut=0, raw=raw, load_clut=1))
    return dict(schema="t172-gpu-polygon-render-experiment-v1", cases=cases)


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
