"""Fresh slope, near-collinear, clipping and coordinate-wrap observations."""
import itertools
import json
import random


def matrix():
    rng = random.Random(1723010)
    cases = []
    def add(vertices, clip, label):
        cases.append(dict(id=f"{label}_{len(cases)}", x=[p[0] for p in vertices],
                          y=[p[1] for p in vertices], clip=clip, doubled=rng.randrange(2),
                          masked_or_blended=rng.randrange(2), interlace=rng.randrange(2),
                          skip_field=rng.randrange(2)))
    for height in range(1, 512):
        for width in [1, 2, height - 1, height, height + 1, 1021, 1022, 1023]:
            for vertices in [[(0, 0), (width, height), (width - 1, height)],
                             [(0, 0), (width, height), (width, 0)]]:
                add(vertices, [0, 0, 1023, 511], "slope")
    for _ in range(8000):
        ox, oy = rng.randrange(-2048, 1024), rng.randrange(-2048, 1536)
        vertices = [(ox + rng.randrange(1024), oy + rng.randrange(512)) for _ in range(3)]
        clip = rng.choice([[0, 0, 1023, 511], [3, 7, 37, 61], [37, 61, 3, 7]])
        add(vertices, clip, "translated")
    for ox, oy in itertools.product([-2048, -1025, -1024, -1, 0, 1020, 1535], repeat=2):
        vertices = [(ox, oy), (ox + 511, oy + 511), (ox + 510, oy + 255)]
        for order in itertools.permutations(range(3)):
            add([vertices[i] for i in order], [0, 0, 1023, 511], "phase")
    return dict(schema="t172-gpu-polygon-experiment-v1", matrix_revision=2,
                cases=cases, expected_outputs=None,
                policy="Post-fit holdouts; compare every ordered span argument and work value")


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
