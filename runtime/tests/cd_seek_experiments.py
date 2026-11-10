"""Independent input grid for the deterministic CD seek compatibility model.

Hardware documentation supplies state and address meanings, not this model's
numerical delays. This matrix contains no expected implementation constants.
"""
import json


def matrix():
    cases = []

    def add(origin, target, motor, paused, mode, profile, label):
        cases.append(dict(id=f"{label}_{len(cases)}", origin=origin, target=target,
                          motor_on=motor, paused=paused, mode=mode, profile=profile))

    distances = list(range(257)) + [450, 750, 1125, 2250, 4500, 9000, 18000,
                                    45000, 90000, 180000, 300000, 450000]
    for profile in [0, 1]:
        for motor in [0, 1]:
            for paused in [0, 1]:
                for mode in [0, 128]:
                    for distance in distances:
                        add(0, distance, motor, paused, mode, profile, "curve")
                        add(distance, 0, motor, paused, mode, profile, "reverse")
                for mode in range(256):
                    for distance in [0, 1, 74, 75, 4500, 45000, 200000]:
                        add(0, distance, motor, paused, mode, profile, "mode")
                for origin in [-150, 0, 150, 75000, 200000]:
                    for distance in [0, 1, 75, 4500, 45000]:
                        add(origin, origin + distance, motor, paused, 0, profile, "translated")
    for profile in [0, 1]:
        for motor in [0, 1]:
            for paused in [0, 1]:
                for mode in [0, 128]:
                    for distance in range(257, 5001):
                        add(0, distance, motor, paused, mode, profile, "threshold")
                    for origin, target in [(-2147483648, 2147483647), (2147483647, -2147483648),
                                           (-2147483648, -2147483648), (2147483647, 2147483647),
                                           (0, 20000000), (0, 21000000), (0, 2147483647)]:
                        add(origin, target, motor, paused, mode, profile, "representation")
    # Holdout inputs chosen after the model was fitted, with no expected outputs.
    import random
    rng = random.Random(1722009)
    for index in range(20000):
        if index < 10000:
            origin, target = rng.randrange(-150, 450001), rng.randrange(-150, 450001)
        else:
            origin, target = rng.randrange(-2147483648, 2147483648), rng.randrange(-2147483648, 2147483648)
        add(origin, target, rng.randrange(2), rng.randrange(2), rng.randrange(256),
            rng.randrange(2), "holdout")
    for motor in [0, 1]:
        boundary = (2147483647 - 10160640 - (0 if motor else 33868800)) * 15 // 1568
        for delta in range(-32, 33):
            for sign in [-1, 1]:
                for mode in [0, 128, 255]:
                    add(0, sign * (boundary + delta), motor, 1, mode, 1, "saturation")
    return dict(schema="t172-cd-seek-experiment-v1", matrix_revision=3,
                domain="Disc-range -150..450000 plus separate signed32 representation stress; boolean states; all mode bits",
                expected_outputs=None, cases=cases)


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
