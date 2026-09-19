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
    return dict(schema="t172-cd-seek-experiment-v1", matrix_revision=2,
                domain="Signed sector addresses -150..450000; boolean states; all mode bits",
                expected_outputs=None, cases=cases)


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
