"""Independent triangle inputs for ordered span and work observations."""
import itertools
import json
import random


def matrix():
    cases = []

    def add(vertices, clip, flags, label):
        cases.append(dict(id=f"{label}_{len(cases)}", x=[v[0] for v in vertices],
                          y=[v[1] for v in vertices], clip=clip,
                          doubled=flags[0], masked_or_blended=flags[1],
                          interlace=flags[2], skip_field=flags[3]))

    triangles = [
        [(0, 0), (8, 0), (0, 8)], [(0, 0), (8, 8), (0, 8)],
        [(0, 0), (8, 0), (4, 7)], [(4, 0), (8, 7), (0, 7)],
        [(0, 0), (7, 3), (2, 9)], [(7, 0), (0, 3), (5, 9)],
        [(0, 0), (1, 9), (2, 0)], [(0, 0), (9, 1), (0, 2)],
        [(0, 0), (2, 2), (4, 4)], [(3, 3), (3, 3), (3, 3)],
        [(-3, -3), (9, 0), (0, 9)], [(1020, 500), (1030, 500), (1020, 510)],
        [(-2048, 0), (-2040, 0), (-2048, 8)],
        [(0, -2048), (8, -2048), (0, -2040)],
        [(0, 0), (1023, 0), (0, 511)], [(0, 0), (1024, 0), (0, 511)],
        [(0, 0), (1023, 0), (0, 512)], [(2040, 0), (2046, 0), (2040, 8)],
        [(0, 1020), (8, 1020), (0, 1030)], [(0, 2040), (8, 2040), (0, 2046)],
    ]
    clips = [[0, 0, 1023, 511], [0, 0, 7, 7], [2, 2, 5, 5],
             [5, 5, 2, 2], [1020, 500, 1023, 511], [0, 0, 0, 0]]
    flags = list(itertools.product(range(2), repeat=4))
    for vertices in triangles:
        for order in itertools.permutations(range(3)):
            for clip in clips:
                for options in flags:
                    add([vertices[i] for i in order], clip, options, "catalog")
    rng = random.Random(1723009)
    for _ in range(2000):
        origin = (rng.randrange(-2048, 2027), rng.randrange(-2048, 2027))
        vertices = [(origin[0] + rng.randrange(20), origin[1] + rng.randrange(20)) for _ in range(3)]
        add(vertices, rng.choice(clips), rng.choice(flags), "translated")
    for _ in range(2000):
        vertices = [(rng.randrange(-20, 60), rng.randrange(-20, 60)) for _ in range(3)]
        add(vertices, [0, 0, 63, 63], rng.choice(flags), "local")
    return dict(schema="t172-gpu-polygon-experiment-v1", matrix_revision=1, cases=cases,
                expected_outputs=None, policy="Compare callback order and all fields, work and cost; retain rejections")


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
