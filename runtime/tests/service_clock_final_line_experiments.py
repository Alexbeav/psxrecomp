"""Independent extension of reviewer-supplied final-scanline holdout inputs."""
import json


def matrix():
    cases = []
    for mode in [0, 32, 36]:
        for start in [0, 550000, 600000]:
            for elapsed in range(500000, 551000, 1000):
                cases.append(dict(id=f'boundary_{mode}_{start}_{elapsed}', operations=[
                    dict(op='gp1', word=0x08000000 | mode), dict(op='to', now=start),
                    dict(op='raster', elapsed=elapsed)]))
    return dict(schema='t172-service-clock-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
