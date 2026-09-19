"""Input-only synthetic ADPCM experiments; no retail or source-derived vectors."""
import json
from spu_envelope_experiments import write, tick

VOICE = 0x1F801C10


def block(header, flags, nibbles):
    assert 0 <= header <= 255 and 0 <= flags <= 255
    assert len(nibbles) == 28 and all(0 <= n < 16 for n in nibbles)
    return [header, flags] + [nibbles[i] | (nibbles[i + 1] << 4) for i in range(0, 28, 2)]


def matrix():
    cases = []
    patterns = {"ramp": [i % 16 for i in range(28)],
                "impulse": [1] + [0] * 27,
                "extremes": [7, 8] * 14,
                "zero": [0] * 28}

    def add(name, blocks, start=0x2000, pitch=0x1000, extra=None, irq=None):
        preload = [{"address": 0x1000, "bytes": block(12, 7, [0] * 28)}]
        for i, data in enumerate(blocks):
            preload.append({"address": (start + i * 16) & 0x7FFFF, "bytes": data})
        ops = [write(0x1F801DAA, 0xC000 if irq is None else 0xC040),
               write(0x1F801D80, 0x3FFF), write(0x1F801D82, 0x3FFF),
               write(VOICE, 0x3FFF), write(VOICE + 2, 0x3FFF),
               write(VOICE + 4, pitch), write(VOICE + 6, start // 8),
               write(VOICE + 8, 0x7F0F), write(VOICE + 10, 0x1FC0),
               write(VOICE + 14, 0x200)]
        if irq is not None:
            ops += [write(0x1F801DA4, irq // 8)]
        ops += [write(0x1F801D88, 2), tick(6), write(VOICE + 12, 0x6000)]
        ops += extra if extra is not None else [tick(120)]
        cases.append({"id": name, "preload": preload, "operations": ops})

    warmup = block(0, 0, [7, 8, 1, 15] * 7)
    silence = block(12, 7, [0] * 28)
    for predictor in range(16):
        for shift in [0, 1, 4, 8, 9, 11, 12, 13, 14, 15]:
            for name, values in patterns.items():
                tested = block(predictor * 16 + shift, 0, values)
                add(f"decode_{predictor:x}_{shift:x}_{name}", [warmup, tested, tested, silence])

    for flags in list(range(8)) + [8, 16, 32, 64, 128, 255]:
        for pitch in [0, 0x800, 0x1000, 0x2000, 0x4000]:
            add(f"flags_{flags:02x}_pitch_{pitch:04x}",
                [block(8, flags, patterns["ramp"]),
                 block(8, 3, patterns["extremes"])], pitch=pitch)

    for start in [0x2000, 0x2008, 0x7FFE0, 0x7FFF0]:
        for irq in [start & ~7, (start + 8) & 0x7FFFF, (start + 16) & 0x7FFFF, 0x3000]:
            add(f"address_{start:05x}_irq_{irq:05x}",
                [block(8, 4, patterns["ramp"]), block(8, 3, patterns["extremes"])],
                start=start, irq=irq)

    for delay in [0, 1, 3, 7, 14, 21, 27, 28, 35, 56]:
        for action, register, value in [("repeat", VOICE + 14, 0x200),
                                         ("rekey", 0x1F801D88, 2),
                                         ("noise", 0x1F801D94, 2)]:
            extra = ([tick(delay)] if delay else []) + [write(register, value), tick(96)]
            add(f"during_{action}_{delay}",
                [block(8, 4, patterns["ramp"]), block(8, 3, patterns["extremes"])], extra=extra)

    assert len({c["id"] for c in cases}) == len(cases)
    return {"schema": "t172-spu-experiment-v1", "matrix_revision": "adpcm-2",
            "reset": "cold core and zero RAM per case, then case synthetic preload",
            "preload": [], "audio": True,
            "observe": [VOICE + 12, VOICE + 14, 0x1F801D9C, 0x1F801D9E,
                        0x1F801DAE, 0x1F801E04, 0x1F801E06, 0x1F801DB8, 0x1F801DBA],
            "observation_timing": "reset, each write, each completed stereo sample",
            "audio_timing": "signed16 stereo; zero on reset, hold last output on writes",
            "expected_outputs": None, "cases": cases}


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
